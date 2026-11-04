/**
 * @file    Massmore_TCS34725_Color.cpp
 * @brief   คณิตสี: RGB8, HSV, Lux / CCT (ams DN40), saturation และ calibration
 *
 * ทุกฟังก์ชันในไฟล์นี้เป็น pure function (ไม่แตะ I2C) — ใช้ gain / ATIME ปัจจุบันจาก shadow
 * ถ้า sketch ไม่เรียกใช้ linker จะตัดโค้ดส่วนนี้ทิ้ง (ประหยัด Flash บน Arduino Nano)
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 * SPDX-License-Identifier: MIT
 */
#include "Massmore_TCS34725.h"
#include <math.h>

namespace {

/// ค่าช่องสีหลังหัก IR (DN40: R' = R − IR …) — ค่าอาจติดลบได้ในแสงที่มี IR สูง
struct IrFree { float r, g, b; };

IrFree removeIR(const Massmore_TCS34725::RawData &d, float ir) {
  IrFree o;
  o.r = (float)d.r - ir;
  o.g = (float)d.g - ir;
  o.b = (float)d.b - ir;
  return o;
}

uint8_t to8(float x) {
  if (!(x > 0.0f)) return 0;              // จับ NAN ด้วย
  if (x >= 255.0f) return 255;
  return (uint8_t)(x + 0.5f);
}

/// gamma 2.5 สำหรับ RGB LED (ตาคนรับรู้ความสว่างแบบไม่เป็นเชิงเส้น)
uint8_t gamma8(uint8_t v) {
  return to8(powf((float)v / 255.0f, 2.5f) * 255.0f);
}

} // namespace

// ===========================================================================
//  Saturation (DN40)
// ===========================================================================

uint16_t Massmore_TCS34725::getSaturationLevel() const {
  // analog saturation = 1024 × cycles (cycles < 64), digital saturation = 65535
  uint32_t sat = getMaxCount();
  // ripple saturation: integration < 150 ms ให้ใช้ 75% ของค่าเต็ม (DN40)
  if (getIntegrationTimeMs() < 150.0f) sat -= sat / 4u;
  return (uint16_t)sat;
}

bool Massmore_TCS34725::isSaturated(const RawData &d) const {
  return d.c >= getSaturationLevel();
}

float Massmore_TCS34725::estimateIR(const RawData &d) const {
  const uint32_t sum = (uint32_t)d.r + d.g + d.b;
  return (sum > d.c) ? (float)(sum - d.c) / 2.0f : 0.0f;
}

// ===========================================================================
//  Lux / CCT (ams DN40)
// ===========================================================================

float Massmore_TCS34725::calculateLux(const RawData &d) const {
  if (isSaturated(d)) return NAN;
  const IrFree p = removeIR(d, estimateIR(d));
  // G'' = R_Coef·R' + G_Coef·G' + B_Coef·B'
  const float g2 = _rCoef * p.r + _gCoef * p.g + _bCoef * p.b;
  // CPL = (ATIME_ms × AGAINx) / (GA × DF)   (counts per lux)
  const float cpl = (getIntegrationTimeMs() * getGainValue()) / (_ga * _df);
  if (!(cpl > 0.0f)) return NAN;
  const float lux = g2 / cpl;
  return (lux < 0.0f) ? 0.0f : lux;
}

float Massmore_TCS34725::calculateColorTemperature(const RawData &d, CctMethod m) const {
  if (isSaturated(d)) return NAN;
  const IrFree p = removeIR(d, estimateIR(d));

  if (m == CctMethod::DN40) {
    // CCT = CT_Coef × B' / R' + CT_Offset
    if (!(p.r > 0.0f)) return NAN;
    return _ctCoef * p.b / p.r + _ctOffset;
  }

  // McCamy: RGB → CIE XYZ → xy → CCT (ค่าคงที่ matrix จากการวัด TCS3472 ในแสงมาตรฐาน)
  const float X = -0.14282f * p.r + 1.54924f * p.g + -0.95641f * p.b;
  const float Y = -0.32466f * p.r + 1.57837f * p.g + -0.73191f * p.b;
  const float Z = -0.68202f * p.r + 0.77073f * p.g +  0.56332f * p.b;
  const float sum = X + Y + Z;
  if (!(sum > 0.0f)) return NAN;
  const float x = X / sum;
  const float y = Y / sum;
  const float den = 0.1858f - y;
  if (den == 0.0f) return NAN;
  const float n = (x - 0.3320f) / den;
  return 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;
}

Massmore_TCS34725::LightData Massmore_TCS34725::calculateLight(const RawData &d) const {
  LightData ld;
  ld.ir = estimateIR(d);
  ld.saturated = isSaturated(d);
  if (ld.saturated) {
    ld.lux = NAN;
    ld.cct = NAN;
    ld.valid = false;
    return ld;
  }
  ld.lux = calculateLux(d);
  ld.cct = calculateColorTemperature(d, CctMethod::DN40);
  ld.valid = (d.c > 0) && !isnan(ld.lux) && !isnan(ld.cct);
  return ld;
}

// ===========================================================================
//  RGB8 / HSV
// ===========================================================================

Massmore_TCS34725::RGB8 Massmore_TCS34725::toRGB8(const RawData &d, bool gamma, uint16_t fullScale) const {
  RGB8 o = { 0, 0, 0 };
  float scale;
  if (fullScale == 0) {
    // normalize ด้วยช่องที่มากที่สุด → ได้ "เฉดสี" เต็มความสด
    uint16_t m = d.r;
    if (d.g > m) m = d.g;
    if (d.b > m) m = d.b;
    if (m == 0) return o;
    scale = 255.0f / (float)m;
  } else {
    // เทียบกับค่าเต็ม (เช่น white reference = 10000) → ได้ทั้งเฉดสีและความสว่างจริง
    scale = 255.0f / (float)fullScale;
  }
  o.r = to8((float)d.r * scale);
  o.g = to8((float)d.g * scale);
  o.b = to8((float)d.b * scale);
  if (gamma) {
    o.r = gamma8(o.r);
    o.g = gamma8(o.g);
    o.b = gamma8(o.b);
  }
  return o;
}

Massmore_TCS34725::HSV Massmore_TCS34725::toHSV(const RawData &d, uint16_t fullScale) const {
  HSV o = { 0.0f, 0.0f, 0.0f };
  const float r = d.r, g = d.g, b = d.b;
  float mx = r, mn = r;
  if (g > mx) mx = g;
  if (b > mx) mx = b;
  if (g < mn) mn = g;
  if (b < mn) mn = b;
  const float delta = mx - mn;

  if (delta > 0.0f) {
    float h;
    if (mx == r)      h = 60.0f * fmodf((g - b) / delta, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / delta + 2.0f);
    else              h = 60.0f * ((r - g) / delta + 4.0f);
    if (h < 0.0f) h += 360.0f;
    o.h = h;
  }
  o.s = (mx > 0.0f) ? delta / mx : 0.0f;

  float v;
  if (fullScale > 0) v = mx / (float)fullScale;
  else               v = (float)d.c / (float)getMaxCount();
  o.v = (v > 1.0f) ? 1.0f : v;
  return o;
}

// ===========================================================================
//  Calibration
// ===========================================================================

void Massmore_TCS34725::setGlassAttenuation(float ga) { if (ga > 0.0f) _ga = ga; }
void Massmore_TCS34725::setDeviceFactor(float df)     { if (df > 0.0f) _df = df; }

void Massmore_TCS34725::setLuxCoefficients(float rCoef, float gCoef, float bCoef) {
  _rCoef = rCoef;
  _gCoef = gCoef;
  _bCoef = bCoef;
}

void Massmore_TCS34725::setCctCoefficients(float ctCoef, float ctOffset) {
  _ctCoef = ctCoef;
  _ctOffset = ctOffset;
}

void Massmore_TCS34725::setDarkOffset(const RawData &dark) {
  _dark = dark;
  _darkSens = _sensitivity();
  _hasDark = true;
}

void Massmore_TCS34725::setWhiteReference(const RawData &white) {
  _white = white;
  _whiteSens = _sensitivity();
  _hasWhite = true;
}

void Massmore_TCS34725::clearCalibration() {
  _hasDark = false;
  _hasWhite = false;
}

Massmore_TCS34725::RawData Massmore_TCS34725::applyCalibration(const RawData &d) const {
  // ค่า dark / white ถูก scale ตามอัตราส่วน sensitivity (cycles × gain) ปัจจุบันเทียบกับตอนบันทึก
  // จึงยังใช้ได้แม้ autoRange() เปลี่ยน gain / ATIME ภายหลัง
  const float cur = _sensitivity();
  const float kDark  = _hasDark  ? cur / _darkSens  : 0.0f;
  const float kWhite = _hasWhite ? cur / _whiteSens : 0.0f;

  const uint16_t in[4] = { d.c, d.r, d.g, d.b };
  const uint16_t dk[4] = { _dark.c, _dark.r, _dark.g, _dark.b };
  const uint16_t wh[4] = { _white.c, _white.r, _white.g, _white.b };
  uint16_t out[4];

  for (uint8_t i = 0; i < 4; i++) {
    const float darkNow = (float)dk[i] * kDark;
    float v = (float)in[i] - darkNow;
    if (v < 0.0f) v = 0.0f;
    if (_hasWhite) {
      float w = (float)wh[i] * kWhite - darkNow;
      if (w < 1.0f) w = 1.0f;
      v = v * (float)MASSMORE_TCS34725_CAL_SCALE / w;
    }
    out[i] = (v >= 65535.0f) ? 65535u : (uint16_t)(v + 0.5f);
  }
  RawData o = { out[0], out[1], out[2], out[3] };
  return o;
}
