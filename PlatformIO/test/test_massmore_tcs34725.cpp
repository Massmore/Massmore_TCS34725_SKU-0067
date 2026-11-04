// Host-side test suite for Massmore_TCS34725
// รันบน PC / Mac ได้เลย ไม่ต้องมีบอร์ดหรือเซ็นเซอร์:  cd PlatformIO/test && make
//
// จำลองชิป TCS34725 ตาม datasheet DS000390: command byte (repeated / auto-increment /
// special function 0xE6), PON → AEN → RGBC init 2.4 ms + integration, AVALID, AINT +
// persistence, WEN/WLONG และ max count 1024 × cycles แล้วตรวจว่าไลบรารีทำงานถูกต้อง

#include <Arduino.h>
#include <Wire.h>
#include "Massmore_TCS34725.h"

// ===========================================================================
//  Simulated time
// ===========================================================================
static uint64_t g_us = 1000000;                       // เริ่มที่ 1 s
unsigned long micros() { g_us += 3; return (unsigned long)g_us; }
unsigned long millis() { g_us += 3; return (unsigned long)(g_us / 1000); }
void delay(unsigned long ms) { g_us += (uint64_t)ms * 1000; }
void delayMicroseconds(unsigned int us) { g_us += us; }
static int g_pinState[64];
void pinMode(int, int) {}
void digitalWrite(int p, int v) { if (p >= 0 && p < 64) g_pinState[p] = v; }
int  digitalRead(int p) { return g_pinState[p]; }

// ===========================================================================
//  Simulated TCS34725
// ===========================================================================
struct Chip {
  bool     present = true;
  uint8_t  addr = 0x29;
  uint8_t  id = 0x44;
  uint8_t  reg[32] = {0};
  uint8_t  ptr = 0;
  bool     autoInc = false;
  uint32_t stepUs = 2400;               // oscillator: 2400 typ, ลองเปลี่ยนเพื่อทดสอบ isGenuine
  uint64_t cycleStart = 0;              // เวลาที่ AEN ถูกตั้ง
  uint64_t lastDone = 0;                // จำนวนรอบที่เสร็จแล้ว
  uint8_t  persCount = 0;
  // แสง: counts ต่อ cycle ที่ gain 1×
  float    rate[4] = { 50, 20, 18, 14 }; // C, R, G, B
  uint32_t blockReads = 0, writesOutsideMask = 0;

  uint8_t  atime() const { return reg[0x01]; }
  uint32_t cycles() const { return 256u - reg[0x01]; }
  float    gain() const { static const float g[4] = {1, 4, 16, 60}; return g[reg[0x0F] & 3]; }
  bool     on(uint8_t bit) const { return reg[0x00] & bit; }

  uint64_t periodUs() const {
    uint64_t steps = 1 + cycles();
    if (on(0x08)) steps += (uint64_t)(256u - reg[0x03]) * ((reg[0x0D] & 2) ? 12 : 1);
    return steps * stepUs;
  }
  void completeCycle() {
    const uint32_t maxc = cycles() >= 64 ? 65535u : 1024u * cycles();
    uint16_t v[4];
    for (int i = 0; i < 4; i++) {
      float x = rate[i] * gain() * (float)cycles();
      v[i] = x >= maxc ? (uint16_t)maxc : (uint16_t)x;
      reg[0x14 + 2 * i] = v[i] & 0xFF;
      reg[0x15 + 2 * i] = v[i] >> 8;
    }
    reg[0x13] |= 0x01;  // AVALID
    const uint16_t ailt = reg[0x04] | (reg[0x05] << 8), aiht = reg[0x06] | (reg[0x07] << 8);
    static const uint8_t persReq[16] = {0, 1, 2, 3, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
    const uint8_t apers = reg[0x0C] & 0x0F;
    const bool out = (v[0] < ailt) || (ailt <= aiht && v[0] > aiht);
    if (apers == 0) reg[0x13] |= 0x10;
    else if (out) { if (++persCount >= persReq[apers]) reg[0x13] |= 0x10; }
    else persCount = 0;
  }
  void sim() {
    if (!(on(0x01) && on(0x02))) return;
    const uint64_t done = (g_us - cycleStart) / periodUs();
    while (lastDone < done) { lastDone++; completeCycle(); }
  }
  void writeReg(uint8_t r, uint8_t v) {
    static const uint8_t mask[32] = {0x1B, 0xFF, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0x0F, 0x02, 0, 0x03};
    if (r < 16 && (v & ~mask[r])) writesOutsideMask++;
    if (r == 0x00) {
      sim();
      const bool wasAen = on(0x01) && on(0x02);
      reg[0] = v;
      const bool isAen = on(0x01) && on(0x02);
      if (isAen && !wasAen) { cycleStart = g_us; lastDone = 0; reg[0x13] &= ~0x01; persCount = 0; }
      if (!isAen) reg[0x13] &= ~0x01;
      return;
    }
    if (r == 0x12 || r == 0x13 || r >= 0x14) return;   // read-only
    reg[r] = v;
  }
  uint8_t readReg(uint8_t r) { sim(); return r == 0x12 ? id : reg[r]; }
} chip;

TwoWire Wire;
void TwoWire::beginTransmission(uint8_t a) { _txAddr = a; _txLen = 0; }
size_t TwoWire::write(uint8_t b) { if (_txLen < 32) _tx[_txLen++] = b; return 1; }
uint8_t TwoWire::endTransmission(bool) {
  g_us += 100;                                          // ~1 transaction @ 400 kHz
  if (!chip.present || _txAddr != chip.addr) return 2;  // NACK on address
  if (_txLen == 0) return 0;
  const uint8_t cmd = _tx[0];
  if ((cmd & 0x80) == 0) return 3;
  const uint8_t type = (cmd >> 5) & 3;
  if (type == 3) {                                      // special function
    if ((cmd & 0x1F) == 0x06) chip.reg[0x13] &= ~0x10;
    return 0;
  }
  chip.ptr = cmd & 0x1F;
  chip.autoInc = (type == 1);
  for (uint8_t i = 1; i < _txLen; i++) {
    chip.writeReg(chip.ptr, _tx[i]);
    if (chip.autoInc) chip.ptr++;
  }
  return 0;
}
uint8_t TwoWire::requestFrom(uint8_t a, uint8_t len) {
  g_us += 100;
  if (!chip.present || a != chip.addr) return 0;
  if (chip.autoInc && chip.ptr == 0x14 && len == 8) chip.blockReads++;
  uint8_t p = chip.ptr;
  for (uint8_t i = 0; i < len; i++) { _rx[i] = chip.readReg(p); if (chip.autoInc) p++; }
  _rxLen = len; _rxPos = 0;
  return len;
}
int TwoWire::read() { return _rxPos < _rxLen ? _rx[_rxPos++] : -1; }
int TwoWire::available() { return _rxLen - _rxPos; }

// ===========================================================================
//  Tiny test framework
// ===========================================================================
static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))
static void resetChip() { Chip fresh; chip = fresh; }

typedef Massmore_TCS34725 TCS;

int main() {
  // ---------------- begin / identity ----------------
  printf("[begin / identity]\n");
  {
    resetChip();
    TCS s;
    CHECK(s.begin());
    CHECK(s.getVariant() == TCS::Variant::TCS34725);
    CHECK(chip.reg[0x00] == 0x13);                    // PON | AEN | AIEN (LED ผ่าน INT ดับเป็นค่าเริ่มต้น)
    CHECK(!s.getLED());
    CHECK(chip.reg[0x01] == 0xC0);                    // default 64 cycles
    CHECK(chip.reg[0x0F] == 0x01);                    // default 4×
    CHECK(s.getSerialNumber() == 0);
    CHECK(s.verifyChipID());
  }
  { resetChip(); chip.present = false; TCS s; CHECK(!s.begin()); CHECK(s.lastError() == TCS::ErrorCode::NOT_FOUND); }
  { resetChip(); chip.id = 0x10; TCS s; CHECK(!s.begin()); CHECK(s.lastError() == TCS::ErrorCode::WRONG_ID); }
  { resetChip(); chip.id = 0x4D; TCS s; CHECK(s.begin()); CHECK(s.getVariant() == TCS::Variant::TCS34727); }
  { resetChip(); chip.addr = 0x39; TCS s; CHECK(!s.begin()); CHECK(s.begin(Wire, 0x39)); }
  {
    // ตั้งค่าก่อน begin() → ต้องถูกเขียนตอน begin (ชิปไม่มี soft-reset)
    resetChip(); chip.reg[0x01] = 0x12; chip.reg[0x0F] = 0x03;   // ค่าค้างจากรอบก่อน
    TCS s;
    CHECK(s.setIntegrationTime(24.0f));
    s.setGain(TCS::Gain::X16);
    CHECK(s.begin());
    CHECK(chip.reg[0x01] == 0xF6);
    CHECK(chip.reg[0x0F] == 0x02);
  }
  { TCS s; TCS::RawData d; CHECK(!s.readRaw(d)); CHECK(s.lastError() == TCS::ErrorCode::NOT_READY); }

  // ---------------- blocking read ----------------
  printf("[blocking read]\n");
  {
    resetChip();
    TCS s; s.begin();
    TCS::RawData d;
    const uint64_t t0 = g_us;
    CHECK(s.readRaw(d));
    const double ms = (g_us - t0) / 1000.0;
    CHECK(d.c == 50 * 4 * 64 && d.r == 20 * 4 * 64 && d.g == 18 * 4 * 64 && d.b == 14 * 4 * 64);
    CHECK(chip.blockReads == 1);                      // 8 ไบต์ใน transaction เดียว
    CHECK(ms > 65 * 2.4 - 1 && ms < 65 * 2.4 + 5);    // init + 64 cycles
    // ข้อมูลใหม่ทุกครั้ง: เปลี่ยนแสงแล้วอ่านทันทีต้องได้ค่าใหม่
    chip.rate[0] = 100;
    CHECK(s.readRaw(d) && d.c == 100 * 4 * 64);
    CHECK(s.readClear() == 100 * 4 * 64);
    CHECK(chip.writesOutsideMask == 0);
  }
  {
    resetChip(); TCS s; s.begin();
    chip.stepUs = 10000;                              // ช้ากว่าสเปคมาก → ต้อง TIMEOUT
    TCS::RawData d;
    CHECK(!s.readRaw(d));
    CHECK(s.lastError() == TCS::ErrorCode::TIMEOUT);
  }
  {
    resetChip(); TCS s; s.begin();
    TCS::RawData d;
    CHECK(s.readRawOneShot(d));
    CHECK((chip.reg[0x00] & 0x03) == 0);              // กลับไป sleep
    CHECK(s.readRaw(d));                              // ปลุกเองได้
  }

  // ---------------- configuration ----------------
  printf("[configuration]\n");
  {
    resetChip(); TCS s; s.begin();
    CHECK(s.setIntegrationTime(24.0f) && chip.reg[0x01] == 0xF6);
    CHECK(s.setIntegrationTime(614.4f) && chip.reg[0x01] == 0x00);
    CHECK(s.getMaxCount() == 65535);
    CHECK(!s.setIntegrationTime(700.0f) && s.lastError() == TCS::ErrorCode::INVALID_ARG);
    CHECK(s.setIntegrationCycles(10) && s.getMaxCount() == 10240);
    NEAR(s.getIntegrationTimeMs(), 24.0, 1e-3);
    s.setGain(TCS::Gain::X60);
    CHECK(chip.reg[0x0F] == 0x03 && s.getGainValue() == 60.0f);

    CHECK(s.setWaitTime(100.0f));
    CHECK(chip.reg[0x03] == 256 - 42 && (chip.reg[0x0D] & 2) == 0 && (chip.reg[0x00] & 0x08));
    CHECK(s.setWaitTime(1000.0f));
    CHECK(chip.reg[0x03] == 256 - 35 && (chip.reg[0x0D] & 2));
    NEAR(s.getWaitTimeMs(), 35 * 28.8, 1e-2);
    CHECK(!s.setWaitTime(8000.0f));
    CHECK(s.setWaitTime(0) && (chip.reg[0x00] & 0x08) == 0);

    s.setPersistence(TCS::Persistence::P10);
    CHECK(chip.reg[0x0C] == 5);
    s.setInterruptThresholds(0x1234, 0xABCD);
    CHECK(chip.reg[0x04] == 0x34 && chip.reg[0x05] == 0x12 && chip.reg[0x06] == 0xCD && chip.reg[0x07] == 0xAB);
    s.enableInterrupt(true);
    CHECK(chip.reg[0x00] & 0x10);
    CHECK(chip.writesOutsideMask == 0);
  }

  // ---------------- self tests / authenticity ----------------
  printf("[self test / authenticity]\n");
  {
    resetChip(); TCS s; s.begin();
    s.detachLED();                                    // ทดสอบ register ค่าเริ่มต้นโดยไม่มี LED ใช้ INT
    s.setIntegrationTime(100.8f);
    CHECK(s.selfTestRegisters() == 9);
    CHECK(chip.reg[0x01] == 0xD6);                    // restore แล้ว
    const uint32_t t = s.measureCycleTimeUs(0xF6);
    printf("  cycle time (ATIME 0xF6) = %u us\n", (unsigned)t);
    CHECK(t >= 26400 && t < 27000);
    CHECK(chip.reg[0x01] == 0xD6);
    CHECK(s.selfTestInterrupt());
    CHECK((chip.reg[0x13] & 0x10) == 0);
    CHECK(chip.reg[0x04] == 0 && chip.reg[0x06] == 0xFF && chip.reg[0x07] == 0xFF);
    CHECK(s.isGenuine());
    CHECK(chip.reg[0x00] == 0x03);
  }
  { resetChip(); chip.stepUs = 2000; TCS s; s.begin(); CHECK(!s.isGenuine()); }   // oscillator เร็วเกิน
  { resetChip(); chip.stepUs = 2700; TCS s; s.begin(); CHECK(!s.isGenuine()); }   // oscillator ช้าเกิน
  { resetChip(); chip.stepUs = 2270; TCS s; s.begin(); CHECK(s.isGenuine()); }    // ขอบล่างสเปค
  { resetChip(); chip.stepUs = 2560; TCS s; s.begin(); CHECK(s.isGenuine()); }    // ขอบบนสเปค

  // ---------------- non-blocking FSM ----------------
  printf("[non-blocking FSM]\n");
  {
    resetChip(); TCS s; s.begin();
    s.setIntegrationTime(24.0f);
    CHECK(s.startContinuous());
    int samples = 0, loops = 0;
    const uint64_t tEnd = g_us + 1000000;
    while (g_us < tEnd) { s.update(); loops++; g_us += 200; if (s.isDataReady()) samples++; }
    printf("  continuous 24 ms: %d samples / 1 s, %d loops\n", samples, loops);
    CHECK(samples >= 33 && samples <= 38);            // ≈ 1 / 26.4 ms
    CHECK(s.getReadings().c == 50 * 1 * 4 * 10);
    CHECK(s.getState() == TCS::State::INTEGRATING);
    s.stopContinuous();
    CHECK(s.getState() == TCS::State::IDLE);
  }
  {
    resetChip(); TCS s; s.begin();
    s.powerOff();
    CHECK(s.requestConversion());
    CHECK(s.getState() == TCS::State::WARMUP);
    CHECK((chip.reg[0x00] & 0x02) == 0);              // ยังไม่ตั้ง AEN ระหว่าง warm-up
    int guard = 0;
    while (s.getState() != TCS::State::READY && guard++ < 100000) { s.update(); g_us += 100; }
    CHECK(s.getState() == TCS::State::READY);
    CHECK(s.isDataReady() && !s.isDataReady());
    CHECK(s.getReadings().c == 50 * 4 * 64);
    CHECK((chip.reg[0x00] & 0x03) == 0);              // one-shot จบแล้ว sleep
  }
  {
    resetChip(); TCS s; s.begin();
    chip.stepUs = 10000;
    s.requestConversion();
    int guard = 0;
    while (s.getState() == TCS::State::INTEGRATING && guard++ < 100000) { s.update(); g_us += 500; }
    CHECK(s.getState() == TCS::State::ERROR && s.lastError() == TCS::ErrorCode::TIMEOUT);
  }

  // ---------------- auto-range ----------------
  printf("[auto-range]\n");
  {
    resetChip(); TCS s; s.begin();
    chip.rate[0] = 500; chip.rate[1] = 200; chip.rate[2] = 180; chip.rate[3] = 140;   // แดดจ้า
    CHECK(s.autoRange());
    printf("  bright → gain %.0fx, %u cycles\n", s.getGainValue(), (unsigned)s.getIntegrationCycles());
    CHECK(s.getGainValue() == 1.0f && s.getIntegrationCycles() == 64);
    TCS::RawData d; s.readRaw(d);
    CHECK(!s.isSaturated(d));
  }
  {
    resetChip(); TCS s; s.begin();
    chip.rate[0] = 0.5f; chip.rate[1] = 0.2f; chip.rate[2] = 0.2f; chip.rate[3] = 0.1f;   // มืด
    CHECK(s.autoRange());
    printf("  dark   → gain %.0fx, %u cycles\n", s.getGainValue(), (unsigned)s.getIntegrationCycles());
    CHECK(s.getGainValue() == 60.0f && s.getIntegrationCycles() == 256);
  }
  {
    resetChip(); TCS s; s.begin();
    chip.rate[0] = 2; chip.rate[1] = 1; chip.rate[2] = 1; chip.rate[3] = 1;
    s.startContinuous();
    int changes = 0;
    for (int i = 0; i < 200000 && changes < 10; i++) {
      s.update(); g_us += 500;
      if (s.isDataReady()) { if (!s.autoRangeStep()) break; changes++; }
    }
    printf("  FSM step → gain %.0fx, %u cycles after %d steps\n", s.getGainValue(), (unsigned)s.getIntegrationCycles(), changes);
    CHECK(changes >= 1);
    CHECK(s.getGainValue() == 60.0f);
  }

  // ---------------- color math ----------------
  printf("[color math]\n");
  {
    resetChip(); TCS s; s.begin();                    // 64 cycles (153.6 ms), 4×
    TCS::RawData d = { 1000, 400, 360, 280 };
    // IR = (400+360+280-1000)/2 = 20 ; R'=380 G'=340 B'=260
    // G'' = 0.136*380 + 340 - 0.444*260 = 51.68 + 340 - 115.44 = 276.24
    // CPL = 153.6*4 / 310 = 1.981935 ; lux = 139.38
    NEAR(s.estimateIR(d), 20.0, 1e-4);
    NEAR(s.calculateLux(d), 276.24 / (153.6 * 4.0 / 310.0), 1e-2);
    NEAR(s.calculateColorTemperature(d), 3810.0 * 260.0 / 380.0 + 1391.0, 1e-1);
    float mc = s.calculateColorTemperature(d, TCS::CctMethod::MCCAMY);
    printf("  McCamy CCT = %.0f K\n", mc);
    CHECK(mc > 1000 && mc < 20000);
    s.setGlassAttenuation(2.0f);
    NEAR(s.calculateLux(d), 2.0 * 276.24 / (153.6 * 4.0 / 310.0), 1e-2);

    // saturation
    TCS::RawData sat = { 65535, 20000, 20000, 20000 };
    CHECK(s.isSaturated(sat) && isnan(s.calculateLux(sat)));
    CHECK(!s.calculateLight(sat).valid);
    CHECK(s.calculateLight(d).valid);
    s.setIntegrationCycles(10);                       // 24 ms → max 10240, ripple 75% = 7680
    CHECK(s.getSaturationLevel() == 7680);
    s.setIntegrationCycles(64);
    CHECK(s.getSaturationLevel() == 65535);

    // RGB8 / HSV
    TCS::RawData red = { 1200, 1000, 200, 100 };
    TCS::RGB8 c = s.toRGB8(red);
    CHECK(c.r == 255 && c.g == 51 && c.b == 26);
    TCS::HSV h = s.toHSV(red);
    NEAR(h.h, 60.0 * (100.0 / 900.0), 0.5);  // ใกล้แดง (≈ 6.7°)
    NEAR(h.s, 0.9, 1e-3);
    TCS::RawData blue = { 1200, 100, 200, 1000 };
    NEAR(s.toHSV(blue).h, 240.0 - 60.0 * (100.0 / 900.0), 0.5);
    TCS::RGB8 gm = s.toRGB8(red, true);
    CHECK(gm.r == 255 && gm.g < c.g);
  }

  // ---------------- calibration ----------------
  printf("[calibration]\n");
  {
    resetChip(); TCS s; s.begin();
    TCS::RawData dark = { 10, 4, 4, 2 }, white = { 30010, 10004, 12004, 8002 };
    s.setDarkOffset(dark);
    s.setWhiteReference(white);
    TCS::RawData w = s.applyCalibration(white);
    CHECK(w.c == 10000 && w.r == 10000 && w.g == 10000 && w.b == 10000);
    TCS::RawData gray = { 15010, 5004, 6004, 4002 };
    TCS::RawData g2 = s.applyCalibration(gray);
    CHECK(g2.r == 5000 && g2.g == 5000 && g2.b == 5000);
    TCS::RGB8 rgb = s.toRGB8(g2, false, MASSMORE_TCS34725_CAL_SCALE);
    CHECK(rgb.r == 128 && rgb.g == 128 && rgb.b == 128);
    // เปลี่ยน gain ×4 → raw ×4 แต่ค่าที่ calibrate ต้องเท่าเดิม
    s.setGain(TCS::Gain::X16);
    TCS::RawData gray4 = { 60040, 20016, 24016, 16008 };
    TCS::RawData g3 = s.applyCalibration(gray4);
    CHECK(g3.r == 5000 && g3.g == 5000 && g3.b == 5000);
    TCS::RawData below = { 0, 0, 0, 0 };
    CHECK(s.applyCalibration(below).r == 0);
  }

  // ---------------- interrupt / low power ----------------
  printf("[interrupt]\n");
  {
    resetChip(); TCS s; s.begin();
    s.detachLED();                                    // ใช้ INT เป็น interrupt เอง (ตัวอย่าง 06)
    s.setIntegrationTime(24.0f);
    s.setInterruptThresholds(100, 1000);              // C = 50×4×10 = 2000 > 1000
    s.setPersistence(TCS::Persistence::P3);
    s.enableInterrupt(true);
    s.setWaitTime(500.0f);
    s.clearInterrupt();
    s.enableRGBC(true);
    g_us += 2 * (26400 + 500000);
    CHECK(!s.isInterruptFlag());                      // ยังไม่ครบ 3 รอบ
    g_us += 2 * (26400 + 500000);
    CHECK(s.isInterruptFlag());
    TCS::RawData d;
    CHECK(s.readLatestRaw(d) && d.c == 2000);
    s.clearInterrupt();
    CHECK(!(chip.reg[0x13] & 0x10));
  }

  // ---------------- misc ----------------
  {
    TCS s;
    CHECK(strcmp(TCS::errorToString(TCS::ErrorCode::TIMEOUT), "TIMEOUT") == 0);
    s.attachLED(5, false);
    CHECK(g_pinState[5] == HIGH);                     // active-low: off = HIGH
    s.setLED(true);
    CHECK(g_pinState[5] == LOW);
  }

  // ---------------- LED ผ่าน jumper INT → LED EN ----------------
  printf("[LED via INT]\n");
  {
    resetChip(); TCS s; s.begin();                    // ค่าเริ่มต้น: LED ผ่าน INT, ดับ
    CHECK(!s.getLED());
    s.attachLEDToINT();
    CHECK(!s.getLED());                               // ยังดับอยู่
    s.ledOn();
    CHECK(s.getLED() && (chip.reg[0x00] & 0x10) == 0);
    s.ledToggle();
    CHECK(!s.getLED());
    CHECK(chip.reg[0x00] & 0x10);                     // AIEN
    CHECK(chip.reg[0x0C] == 0);                       // APERS = EVERY
    g_us += 200000;
    CHECK(s.isInterruptFlag());                       // AINT ค้าง → INT ดึงลง → LED ดับ
    TCS::RawData d;
    CHECK(s.readRaw(d));
    CHECK(chip.reg[0x13] & 0x10);                     // อ่านค่าแล้ว LED ยังดับ
    s.setLED(true);
    CHECK((chip.reg[0x00] & 0x10) == 0);              // ปล่อย INT → LED ติด
    s.powerOff();
    s.setLED(false);
    CHECK((chip.reg[0x00] & 0x13) == 0x13);           // เปิด PON + AEN ให้เองเพื่อให้เกิด AINT
  }
  {
    resetChip(); TCS s;
    s.ledOn();                                        // สั่งก่อน begin → มีผลตอน begin
    CHECK(s.begin());
    CHECK(s.getLED() && (chip.reg[0x00] & 0x10) == 0);
  }
  {
    resetChip(); TCS s; s.begin();
    s.detachLED();                                    // คืนขา INT
    CHECK((chip.reg[0x00] & 0x10) == 0);
    CHECK(chip.reg[0x04] == 0 && chip.reg[0x05] == 0 && chip.reg[0x06] == 0xFF && chip.reg[0x07] == 0xFF);
    s.ledOn();                                        // ไม่มี LED ผูกอยู่ → ไม่แตะ INT
    CHECK((chip.reg[0x00] & 0x10) == 0);
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
