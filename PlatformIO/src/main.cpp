/**
 * 07_Factory_Test — Outgoing QA สำหรับ Massmore SKU-0067 (TCS34725)
 *
 * รันอัตโนมัติหลังบูต / พิมพ์ r เพื่อทดสอบซ้ำ — Serial 115200
 * บรรทัดที่ขึ้นต้นด้วย # เป็นรูปแบบที่ Massmore Web Serial Monitor อ่านอัตโนมัติ (ภาษาอังกฤษเท่านั้น)
 *
 * ลำดับการทดสอบ
 *   BUS_SCAN, CHIP_ID   — ด่านกั้น: ไม่ผ่านจะไม่ begin() / ไม่ทดสอบต่อ
 *   SERIAL              — ชิปไม่มี serial number → N/A
 *   REG_RW              — เขียน/อ่านกลับ 9 register
 *   INIT_TIMING         — เวลา AEN → AVALID ที่ 10 cycles ต้องอยู่ใน 11 × 2.27–2.56 ms (oscillator)
 *   INTERRUPT_LOGIC     — AINT ตั้งเมื่อนอก threshold / ไม่ตั้งเมื่ออยู่ใน / หายหลัง 0xE6
 *   ADC_CEILING         — ATIME 0xFF (1 cycle) + 60× ทุกช่องต้อง ≤ 1024
 *   GAIN_RATIO          — 4× / 16× / 60× เทียบ 1× ต้องอยู่ในช่วง datasheet (±3 %)
 *                         (แสงมากจน 60× อิ่มตัว → ปิด LED แล้ววัดด้วยแสงรอบข้าง)
 *   AUTHENTICITY        — GENUINE เมื่อผ่านทุกข้อด้านบน
 *   LED_BLINK           — กระพริบ LED บนบอร์ด 5 ครั้ง (มองเห็นได้) และวัด clear ตอนเปิด > ตอนปิดทุกครั้ง
 *   CONTINUOUS          — 20 ครั้งติด ไม่มี timeout / saturation / noise ≤ ±5 %
 *   RANGE_CLEAR/LUX/CCT — ค่าที่วัดได้อยู่ในช่วงทางกายภาพ
 *
 * แสง: ใช้ LED บนบอร์ด (jumper INT → LED EN — ไลบรารีดับเป็นค่าเริ่มต้น sketch นี้สั่ง ledOn() เอง)
 *       ให้หน้าเซ็นเซอร์อยู่ในที่แสงนิ่ง
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

// ---------------- Factory wiring (hardcode เฉพาะใน sketch นี้) ----------------
#if defined(ESP32)
  #if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define FT_SDA 8
    #define FT_SCL 9
    #define FT_MCU "ESP32-S3"
  #else
    #define FT_SDA 21               // Qwiic ของ Massmore บน ESP32 classic
    #define FT_SCL 22
    #define FT_MCU "ESP32"
  #endif
#elif defined(__AVR_ATmega328P__)
  #define FT_MCU "ATMEGA328P"
#else
  #define FT_MCU "UNKNOWN"
#endif
#define FT_I2C_HZ    100000UL       // 100 kHz เผื่อสายยาว
#define FT_ADDR      MASSMORE_TCS34725_ADDR_DEFAULT
#define FT_LED_INT   1              // 1 = LED ควบคุมผ่าน jumper INT → LED EN, 0 = ไม่ควบคุม
#define FT_LED_PIN   -1             // ถ้า LED EN ต่อเข้า GPIO ให้ใส่ขา (และตั้ง FT_LED_INT 0)
#define FT_BLINKS    5              // จำนวนครั้งที่กระพริบใน LED_BLINK
#define FT_BLINK_MS  250            // เวลาค้างสถานะเปิด / ปิด ต่อครั้ง (ให้ตามองเห็น)

typedef Massmore_TCS34725 TCS;
TCS tcs;

const __FlashStringHelper *gFailReason = nullptr;

// ---------------------------------------------------------------------------
//  Report helpers
// ---------------------------------------------------------------------------
static void resultHead(const __FlashStringHelper *name, bool pass, const __FlashStringHelper *reason) {
  if (!pass && gFailReason == nullptr) gFailReason = reason;
  Serial.print(F("#RESULT "));
  Serial.print(name);
  Serial.print(pass ? F(" PASS ") : F(" FAIL "));
}

static void printHex2(uint8_t v) {
  Serial.print(F("0x"));
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

static void verdict() {
  if (gFailReason == nullptr) {
    Serial.println(F("#VERDICT PASS"));
    Serial.println(F("[PASS] SENSOR QA PASSED - READY TO SHIP"));
  } else {
    Serial.print(F("#VERDICT FAIL ")); Serial.println(gFailReason);
    Serial.print(F("[FAIL] QA CHECK FAILED: ")); Serial.println(gFailReason);
  }
}

#define FT_HAS_LED (FT_LED_INT || FT_LED_PIN >= 0)

/// เปิด LED ส่องหน้าเซ็นเซอร์ระหว่างทดสอบ (ไลบรารีดับ LED เป็นค่าเริ่มต้นหลัง begin)
static void testLightOn() {
#if FT_HAS_LED
  tcs.ledOn();
#endif
}

/// อ่าน C หลังเปลี่ยนสถานะ LED — ทิ้งรอบแรก (LED ผ่าน INT ดับภายใน 1 cycle) แล้วใช้รอบถัดไป
static bool readClearSettled(uint16_t &c) {
  TCS::RawData d;
  if (!tcs.readRaw(d) || !tcs.readRaw(d)) return false;
  c = d.c;
  return true;
}

/// อ่านเฉลี่ย n ครั้ง (32-bit sum)
static bool readAvg(TCS::RawData &out, uint8_t n) {
  uint32_t c = 0, r = 0, g = 0, b = 0;
  for (uint8_t i = 0; i < n; i++) {
    TCS::RawData d;
    if (!tcs.readRaw(d)) return false;
    c += d.c; r += d.r; g += d.g; b += d.b;
  }
  out.c = c / n; out.r = r / n; out.g = g / n; out.b = b / n;
  return true;
}

/// GAIN_RATIO: ตั้ง 60× แล้วหา cycles มากที่สุดที่ C ยังอยู่ในช่วงเชิงเส้น
///   ≥ 64 cycles (≥ 153.6 ms): เพดาน 65535 → ใช้ได้ถึง 90 %
///   <  64 cycles: ripple saturation 75 % ของ 1024 × cycles → ใช้ได้ถึง 70 %
/// @return false = แม้ 1 cycle ก็ยังสว่างเกิน
static bool pickGainCycles() {
  static const uint16_t kCycles[] = { 256, 128, 64, 32, 16, 8, 4, 2, 1 };
  tcs.setGain(TCS::Gain::X60);
  for (uint8_t i = 0; i < sizeof(kCycles) / sizeof(kCycles[0]); i++) {
    const uint16_t n = kCycles[i];
    tcs.setIntegrationCycles(n);
    const uint32_t limit = (n >= 64) ? 65535UL * 9u / 10u : 1024UL * n * 7u / 10u;
    TCS::RawData d;
    if (tcs.readRaw(d) && d.c < limit) return true;
  }
  return false;
}

/// อ่าน ID ตรงจากบัส (ก่อน begin) — command byte 0x80 | 0x12
static bool rawReadId(uint8_t &id) {
  Wire.beginTransmission(FT_ADDR);
  Wire.write((uint8_t)(MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_REG_ID));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)FT_ADDR, (uint8_t)1) != 1) return false;
  id = (uint8_t)Wire.read();
  return true;
}

// ---------------------------------------------------------------------------
//  Test sequence
// ---------------------------------------------------------------------------
static void runFactoryTest() {
  gFailReason = nullptr;
  Serial.println();
  Serial.println(F("#MASSMORE_FACTORY_TEST v1.0"));
  Serial.println(F("#PRODUCT Massmore_TCS34725"));
  Serial.print(F("#MCU ")); Serial.println(F(FT_MCU));

  // ---------- BUS_SCAN (gate) ----------
  Wire.beginTransmission(FT_ADDR);
  const bool ack = Wire.endTransmission() == 0;
  resultHead(F("BUS_SCAN"), ack, F("NOT_FOUND"));
  if (ack) printHex2(FT_ADDR); else Serial.print(F("NONE"));
  Serial.println();
  if (!ack) { verdict(); return; }

  // ---------- CHIP_ID (gate) ----------
  uint8_t id = 0;
  const bool idRead = rawReadId(id);
  const bool idOk = idRead && id == MASSMORE_TCS34725_ID_TCS34725;
  resultHead(F("CHIP_ID"), idOk,
             (idRead && id == MASSMORE_TCS34725_ID_TCS34727) ? F("WRONG_VARIANT") : F("CHIP_ID_MISMATCH"));
  printHex2(id);
  Serial.println();
  if (!idOk) {
    if (id == 0x00 || id == 0xFF) Serial.println(F("Bus stuck or no pull-up? (ID 0x00/0xFF)"));
    if (id == MASSMORE_TCS34725_ID_TCS34727) Serial.println(F("TCS34727 (1.8 V I2C) is not SKU-0067"));
    verdict();
    return;
  }

  if (!tcs.begin(Wire, FT_ADDR)) {
    resultHead(F("BEGIN"), false, F("BEGIN_FAIL"));
    Serial.println(TCS::errorToString(tcs.lastError()));
    verdict();
    return;
  }
#if FT_LED_INT
  tcs.attachLEDToINT();                              // ค่าเริ่มต้นของไลบรารีอยู่แล้ว เขียนไว้ให้ชัด
#elif FT_LED_PIN >= 0
  tcs.attachLED(FT_LED_PIN);
#else
  tcs.detachLED();
#endif
  testLightOn();

  // ---------- SERIAL ----------
  resultHead(F("SERIAL"), true, nullptr);
  Serial.println(F("N/A"));                          // TCS3472 ไม่มี serial / unique ID

  // ---------- REG_RW ----------
  const uint8_t regs = tcs.selfTestRegisters();
  resultHead(F("REG_RW"), regs == 9, F("REG_RW_FAIL"));
  Serial.print(regs); Serial.println(F("/9"));

  // ---------- INIT_TIMING (ค่าต่ำสุดจาก 3 ครั้ง) ----------
  uint32_t tUs = 0xFFFFFFFFUL;
  for (uint8_t i = 0; i < 3; i++) {
    const uint32_t m = tcs.measureCycleTimeUs(0xF6);
    if (m != 0 && m < tUs) tUs = m;
  }
  const uint32_t tMin = 11UL * MASSMORE_TCS34725_STEP_US_MIN - 500UL;
  const uint32_t tMax = 11UL * MASSMORE_TCS34725_STEP_US_MAX + 1500UL;
  const bool timingOk = tUs >= tMin && tUs <= tMax;
  resultHead(F("INIT_TIMING"), timingOk, F("TIMING_OUT_OF_SPEC"));
  if (tUs == 0xFFFFFFFFUL) Serial.println(F("TIMEOUT"));
  else { Serial.print(tUs / 1000.0f, 1); Serial.println(F("ms")); }

  // ---------- INTERRUPT_LOGIC ----------
  const bool intOk = tcs.selfTestInterrupt();
  resultHead(F("INTERRUPT_LOGIC"), intOk, F("INTERRUPT_FAIL"));
  Serial.println(intOk ? F("AINT_SET_CLEAR") : F("AINT_MISMATCH"));
  testLightOn();                                           // selfTest คืน AIEN เดิม → ยืนยันว่า LED ติด

  // ---------- ADC_CEILING: 1 cycle + 60× → ทุกช่อง ≤ 1024 ----------
  tcs.setATIME(0xFF);
  tcs.setGain(TCS::Gain::X60);
  TCS::RawData d;
  uint16_t ceilMax = 0;
  bool ceilOk = true;
  for (uint8_t i = 0; i < 3; i++) {
    if (!tcs.readRaw(d)) { ceilOk = false; break; }
    const uint16_t ch[4] = { d.c, d.r, d.g, d.b };
    for (uint8_t k = 0; k < 4; k++) if (ch[k] > ceilMax) ceilMax = ch[k];
  }
  ceilOk = ceilOk && ceilMax <= 1024;
  resultHead(F("ADC_CEILING"), ceilOk, F("ADC_CEILING_FAIL"));
  Serial.println(ceilMax);

  // ---------- GAIN_RATIO ----------
  // หา cycles ที่ 60× ยังไม่อิ่มตัว แล้ววัดทุก gain ที่ cycles เดียวกัน
  // แสงมากเกิน (เช่นมือ / วัตถุสะท้อนแสง LED กลับเข้าเซ็นเซอร์) → ปิด LED แล้ววัดด้วยแสงรอบข้างอย่างเดียว
  bool gainFit = pickGainCycles();
  bool gainLedOff = false;
#if FT_HAS_LED
  if (!gainFit) {
    tcs.ledOff();
    tcs.setIntegrationCycles(1);
    tcs.readRaw(d); tcs.readRaw(d);                  // รอให้ AINT ตั้ง → LED ดับจริงก่อนวัด
    gainLedOff = true;
    gainFit = pickGainCycles();
  }
#endif
  float gc[4] = { 0, 0, 0, 0 };
  bool gainRead = true;
  for (uint8_t g = 0; g < 4; g++) {
    tcs.setGain((TCS::Gain)g);
    if (!readAvg(d, 4)) { gainRead = false; break; }
    gc[g] = d.c;
  }
  testLightOn();                                           // คืน LED ให้ขั้นถัดไป
  // datasheet: 4× = 3.8–4.2, 16× = 15–16.8, 60× = 58–63 (เผื่อ ±3 % สำหรับ noise / แสงไม่นิ่ง)
  static const float kLo[4] = { 0, 3.8f, 15.0f, 58.0f };
  static const float kHi[4] = { 0, 4.2f, 16.8f, 63.0f };
  float ratio[4] = { 1, 0, 0, 0 };
  bool gainOk = gainRead && gainFit && gc[0] >= 20.0f;   // 60× ต้องไม่อิ่มตัว และ 1× ต้องมีสัญญาณพอ
  for (uint8_t g = 1; g < 4 && gainRead; g++) {
    ratio[g] = (gc[0] > 0) ? gc[g] / gc[0] : 0;
    if (ratio[g] < kLo[g] * 0.97f || ratio[g] > kHi[g] * 1.03f) gainOk = false;
  }
  const bool gainLight = !gainFit || (gainRead && gc[0] < 20.0f);   // วัดไม่ได้เพราะแสง ไม่ใช่ชิปเสีย
  resultHead(F("GAIN_RATIO"), gainOk,
             !gainFit ? F("LIGHT_TOO_HIGH") : (gainLight ? F("LIGHT_TOO_LOW") : F("GAIN_OUT_OF_SPEC")));
  Serial.print(ratio[1], 1); Serial.print('/');
  Serial.print(ratio[2], 1); Serial.print('/');
  Serial.println(ratio[3], 1);
  if (!gainFit) {
    Serial.println(F("Light too HIGH for gain test - remove hand / object in front of sensor, shade from strong light"));
  } else if (gainRead && gc[0] < 20.0f) {
    Serial.println(F("Light too LOW for gain test - check LED / ambient light"));
  }
  Serial.print(F("  (cycles=")); Serial.print(tcs.getIntegrationCycles());
  Serial.print(F(" C@1x=")); Serial.print(gc[0], 0);
  Serial.print(F(" C@60x=")); Serial.print(gc[3], 0);
  if (gainLedOff) Serial.print(F(" led=off"));
  Serial.println(')');

  // ---------- AUTHENTICITY ----------
  const bool genuine = idOk && regs == 9 && timingOk && intOk && ceilOk && gainOk;
  resultHead(F("AUTHENTICITY"), genuine, F("AUTHENTICITY_SUSPECT"));
  Serial.println(genuine ? F("GENUINE") : (gainLight && !gainOk && idOk && regs == 9 && timingOk && intOk && ceilOk
                                           ? F("INCONCLUSIVE") : F("SUSPECT")));

  // ---------- LED_BLINK: กระพริบ LED แล้ววัดว่าเปิด/ปิดได้จริง ----------
#if FT_HAS_LED
  {
    // 24 ms / 16× — ถ้าตอนเปิด LED อิ่มตัว ลด gain ลงจนไม่อิ่มตัว
    tcs.setIntegrationTime(24.0f);
    tcs.setGain(TCS::Gain::X16);
    for (int8_t g = (int8_t)TCS::Gain::X16; g >= 0; g--) {
      tcs.setGain((TCS::Gain)g);
      testLightOn();
      if (tcs.readRaw(d) && !tcs.isSaturated(d)) break;
    }
    uint8_t blinkGood = 0;
    uint16_t onMin = 0xFFFF, offMax = 0;
    bool blinkRead = true;
    for (uint8_t i = 0; i < FT_BLINKS; i++) {
      uint16_t cOn = 0, cOff = 0;
      tcs.ledOn();
      const uint32_t t0 = millis();
      if (!readClearSettled(cOn)) blinkRead = false;
      while (millis() - t0 < FT_BLINK_MS) delay(1);
      tcs.ledOff();
      const uint32_t t1 = millis();
      if (!readClearSettled(cOff)) blinkRead = false;
      while (millis() - t1 < FT_BLINK_MS) delay(1);
      if (cOn < onMin) onMin = cOn;
      if (cOff > offMax) offMax = cOff;
      // ต่างกันอย่างน้อย 10 % ของค่าตอนปิด + 20 counts (LED เสีย / jumper หลุด → on ≈ off)
      if (blinkRead && (uint32_t)cOn > (uint32_t)cOff + cOff / 10u + 20u) blinkGood++;
    }
    testLightOn();
    const bool blinkOk = blinkRead && blinkGood == FT_BLINKS;
    resultHead(F("LED_BLINK"), blinkOk, F("LED_FAIL"));
    Serial.print(blinkGood); Serial.print('/'); Serial.println(FT_BLINKS);
    Serial.print(F("  (on_min=")); Serial.print(onMin);
    Serial.print(F(" off_max=")); Serial.print(offMax);
    Serial.print(F(" gain=")); Serial.print(tcs.getGainValue(), 0); Serial.println(F("x)"));
    if (!blinkOk) {
      // on - off < 10 % → ส่วนใหญ่คือลืมต่อ jumper INT → LED EN (LED ติดค้าง / ดับค้าง ควบคุมไม่ได้)
      Serial.println(F("  ************************************************************"));
      Serial.println(F("  * WARNING: LED did not blink (ON vs OFF < 10 %)            *"));
      Serial.println(F("  * -> Connect jumper INT -> LED EN pin on the sensor board  *"));
      Serial.println(F("  * -> Then check LED / reduce strong ambient light          *"));
      Serial.println(F("  * -> Send 'r' to run again                                 *"));
      Serial.println(F("  ************************************************************"));
    }
  }
#else
  resultHead(F("LED_BLINK"), true, nullptr);
  Serial.println(F("N/A"));                          // ไม่ได้ต่อ LED ให้ควบคุม
#endif

  // ---------- CONTINUOUS: 20 samples ----------
  tcs.setIntegrationTime(153.6f);
  tcs.setGain(TCS::Gain::X4);
  tcs.autoRange();
  uint8_t good = 0;
  uint32_t sumC = 0, sumR = 0, sumG = 0, sumB = 0;
  uint16_t minC = 0xFFFF, maxC = 0;
  for (uint8_t i = 0; i < 20; i++) {
    if (!tcs.readRaw(d)) continue;
    if (d.c == 0 || tcs.isSaturated(d)) continue;
    good++;
    sumC += d.c; sumR += d.r; sumG += d.g; sumB += d.b;
    if (d.c < minC) minC = d.c;
    if (d.c > maxC) maxC = d.c;
  }
  TCS::RawData mean = { 0, 0, 0, 0 };
  if (good) {
    mean.c = sumC / good; mean.r = sumR / good; mean.g = sumG / good; mean.b = sumB / good;
  }
  // noise: ทุกค่าต้องห่างจากค่าเฉลี่ยไม่เกิน 5 % (+3 counts สำหรับค่าน้อย)
  const uint32_t tol = (uint32_t)mean.c * 5u / 100u + 3u;
  const bool noiseOk = good > 0 && (uint32_t)(maxC - mean.c) <= tol && (uint32_t)(mean.c - minC) <= tol;
  const bool contOk = good == 20 && noiseOk;
  resultHead(F("CONTINUOUS"), contOk, F("CONTINUOUS_FAIL"));
  Serial.print(good); Serial.println(F("/20"));
  if (good == 20 && !noiseOk) {
    Serial.print(F("  noise: min=")); Serial.print(minC);
    Serial.print(F(" max=")); Serial.print(maxC);
    Serial.print(F(" mean=")); Serial.println(mean.c);
  }

  // ---------- RANGE ----------
  const bool clearOk = good > 0 && mean.c > 0 && mean.c < 65535;
  resultHead(F("RANGE_CLEAR"), clearOk, F("RANGE_FAIL"));
  Serial.println(mean.c);

  const TCS::LightData L = tcs.calculateLight(mean);
  const bool luxOk = good > 0 && !isnan(L.lux) && L.lux >= 0.0f;
  resultHead(F("RANGE_LUX"), luxOk, F("RANGE_FAIL"));
  Serial.println(L.lux, 1);

  const bool cctOk = good > 0 && !isnan(L.cct) && L.cct >= 1000.0f && L.cct <= 20000.0f;
  resultHead(F("RANGE_CCT"), cctOk, F("RANGE_FAIL"));
  Serial.println(L.cct, 0);

  verdict();

  // ข้อมูลประกอบสำหรับช่าง (ไม่ขึ้นต้นด้วย #)
  Serial.print(F("info: gain=")); Serial.print(tcs.getGainValue(), 0);
  Serial.print(F("x atime=")); Serial.print(tcs.getIntegrationTimeMs(), 1);
  Serial.print(F("ms R=")); Serial.print(mean.r);
  Serial.print(F(" G=")); Serial.print(mean.g);
  Serial.print(F(" B=")); Serial.println(mean.b);
  Serial.println(F("Send 'r' to run again."));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  delay(200);
#if defined(ESP32)
  Wire.begin(FT_SDA, FT_SCL, FT_I2C_HZ);
#else
  Wire.begin();
  Wire.setClock(FT_I2C_HZ);
#endif
  runFactoryTest();
}

void loop() {
  if (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == 'r' || c == 'R') runFactoryTest();
  }
}
