/**
 * @file    Massmore_TCS34725.cpp
 * @brief   I2C bus, configuration, blocking read, Non-blocking FSM, identity / self-test
 *
 * คณิตสี (RGB8 / HSV / Lux / CCT / Calibration) อยู่ใน Massmore_TCS34725_Color.cpp
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 * SPDX-License-Identifier: MIT
 */
#include "Massmore_TCS34725.h"

// ---------------------------------------------------------------------------
//  Auto-range ladder — เรียงจากไวน้อย (แสงจ้า) ไปไวมาก (แสงน้อย)
//  sensitivity = cycles × gain : 64, 256, 1024, 3840, 15360
//  หมายเหตุ: ไม่มีขั้นที่ cycles < 64 เพราะ analog saturation = 1024 counts/cycle เท่ากันทุก ATIME
//  และ integration < 150 ms ยังโดน ripple 75% — ลด cycles จึงไม่ได้ช่วงวัดเพิ่ม (แค่เร็วขึ้น)
// ---------------------------------------------------------------------------
static const uint8_t  kRangeGain[]   = { 0 /*X1*/, 1 /*X4*/, 2 /*X16*/, 3 /*X60*/, 3 /*X60*/ };
static const uint16_t kRangeCycles[] = { 64, 64, 64, 64, 256 };
static const uint8_t  kRangeCount    = sizeof(kRangeCycles) / sizeof(kRangeCycles[0]);
static const float    kGainNominal[] = { 1.0f, 4.0f, 16.0f, 60.0f };

/// mask ของ bit ที่เขียนได้ในแต่ละ register (reserved bit ต้องเป็น 0)
static uint8_t writableMask(uint8_t reg) {
  switch (reg) {
    case MASSMORE_TCS34725_REG_ENABLE:  return MASSMORE_TCS34725_ENABLE_MASK;
    case MASSMORE_TCS34725_REG_PERS:    return MASSMORE_TCS34725_PERS_MASK;
    case MASSMORE_TCS34725_REG_CONFIG:  return MASSMORE_TCS34725_CONFIG_MASK;
    case MASSMORE_TCS34725_REG_CONTROL: return MASSMORE_TCS34725_CONTROL_AGAIN_MASK;
    default:                            return 0xFF;
  }
}

Massmore_TCS34725::Massmore_TCS34725()
  : _wire(nullptr), _addr(MASSMORE_TCS34725_ADDR_DEFAULT), _err(ErrorCode::NOT_READY),
    _variant(Variant::UNKNOWN), _begun(false), _powered(false),
    _atime(0xC0),          // 64 cycles = 153.6 ms → max count 65535 และพ้นช่วง ripple
    _gain(Gain::X4),
    _wtime(0xFF), _wlong(false), _wen(false),
    _pers(0), _ailt(0), _aiht(0xFFFF), _aien(false),
    _state(State::IDLE), _continuous(false), _newData(false), _t0(0), _data{0, 0, 0, 0},
    _arMinClear(0), _arMaxPct(75),
    _ga(1.0f), _df(310.0f), _rCoef(0.136f), _gCoef(1.0f), _bCoef(-0.444f),
    _ctCoef(3810.0f), _ctOffset(1391.0f),
    _dark{0, 0, 0, 0}, _white{0, 0, 0, 0}, _darkSens(1.0f), _whiteSens(1.0f),
    _hasDark(false), _hasWhite(false),
    _ledPin(-1), _ledActiveHigh(true), _ledViaInt(true), _ledOn(false) {}   // LED ผ่าน INT, ดับ

// ===========================================================================
//  Low-level I2C
// ===========================================================================

bool Massmore_TCS34725::readRegister(uint8_t reg, uint8_t &val) {
  if (_wire == nullptr) { _err = ErrorCode::NOT_READY; return false; }
  _wire->beginTransmission(_addr);
  _wire->write((uint8_t)(MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_CMD_REPEATED | (reg & 0x1F)));
  if (_wire->endTransmission() != 0) { _err = ErrorCode::BUS_ERROR; return false; }
  if (_wire->requestFrom(_addr, (uint8_t)1) != 1) { _err = ErrorCode::BUS_ERROR; return false; }
  val = (uint8_t)_wire->read();
  _err = ErrorCode::OK;
  return true;
}

bool Massmore_TCS34725::writeRegister(uint8_t reg, uint8_t val) {
  if (_wire == nullptr) { _err = ErrorCode::NOT_READY; return false; }
  _wire->beginTransmission(_addr);
  _wire->write((uint8_t)(MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_CMD_REPEATED | (reg & 0x1F)));
  _wire->write(val);
  if (_wire->endTransmission() != 0) { _err = ErrorCode::BUS_ERROR; return false; }
  _err = ErrorCode::OK;
  return true;
}

bool Massmore_TCS34725::_readBlock(uint8_t reg, uint8_t *buf, uint8_t len) {
  if (_wire == nullptr) { _err = ErrorCode::NOT_READY; return false; }
  _wire->beginTransmission(_addr);
  _wire->write((uint8_t)(MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_CMD_AUTO_INC | (reg & 0x1F)));
  if (_wire->endTransmission() != 0) { _err = ErrorCode::BUS_ERROR; return false; }
  if (_wire->requestFrom(_addr, len) != len) { _err = ErrorCode::BUS_ERROR; return false; }
  for (uint8_t i = 0; i < len; i++) buf[i] = (uint8_t)_wire->read();
  _err = ErrorCode::OK;
  return true;
}

bool Massmore_TCS34725::_writeBlock(uint8_t reg, const uint8_t *buf, uint8_t len) {
  if (_wire == nullptr) { _err = ErrorCode::NOT_READY; return false; }
  _wire->beginTransmission(_addr);
  _wire->write((uint8_t)(MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_CMD_AUTO_INC | (reg & 0x1F)));
  for (uint8_t i = 0; i < len; i++) _wire->write(buf[i]);
  if (_wire->endTransmission() != 0) { _err = ErrorCode::BUS_ERROR; return false; }
  _err = ErrorCode::OK;
  return true;
}

/// Read-Modify-Write: reg = (reg & ~mask) | (val & mask) และบังคับ reserved bit = 0
bool Massmore_TCS34725::_updateBits(uint8_t reg, uint8_t mask, uint8_t val) {
  uint8_t v;
  if (!readRegister(reg, v)) return false;
  v = (uint8_t)(((v & ~mask) | (val & mask)) & writableMask(reg));
  return writeRegister(reg, v);
}

bool Massmore_TCS34725::_sendCommand(uint8_t cmd) {
  if (_wire == nullptr) { _err = ErrorCode::NOT_READY; return false; }
  _wire->beginTransmission(_addr);
  _wire->write(cmd);
  if (_wire->endTransmission() != 0) { _err = ErrorCode::BUS_ERROR; return false; }
  _err = ErrorCode::OK;
  return true;
}

/// อ่าน C,R,G,B ด้วย auto-increment 8 ไบต์ — อ่าน low byte ก่อน high byte ถูก latch ใน shadow register
bool Massmore_TCS34725::_readData(RawData &out) {
  uint8_t b[8];
  if (!_readBlock(MASSMORE_TCS34725_REG_CDATAL, b, 8)) return false;
  out.c = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
  out.r = (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
  out.g = (uint16_t)(b[4] | ((uint16_t)b[5] << 8));
  out.b = (uint16_t)(b[6] | ((uint16_t)b[7] << 8));
  return true;
}

bool Massmore_TCS34725::_ensureBegun() {
  if (!_begun) { _err = ErrorCode::NOT_READY; return false; }
  return true;
}

// ===========================================================================
//  Timing
// ===========================================================================

uint32_t Massmore_TCS34725::_cycleSteps() const {
  uint32_t steps = 1u + getIntegrationCycles();            // RGBC init 2.4 ms + integration
  if (_wen) steps += (uint32_t)(256u - _wtime) * (_wlong ? MASSMORE_TCS34725_WLONG_FACTOR : 1u);
  return steps;
}

uint32_t Massmore_TCS34725::_timeoutMs() const {
  // ใช้ step สูงสุดตาม datasheet (2.56 ms) + margin 10 ms สำหรับ I2C / scheduler
  return (_cycleSteps() * MASSMORE_TCS34725_STEP_US_MAX + 999u) / 1000u + 10u;
}

uint32_t Massmore_TCS34725::_minCycleMs() const {
  // เร็วที่สุดที่ AVALID จะตั้งได้หลัง AEN (step ต่ำสุด 2.27 ms, ไม่รวม wait)
  return ((1u + getIntegrationCycles()) * MASSMORE_TCS34725_STEP_US_MIN) / 1000u;
}

// ===========================================================================
//  Lifecycle
// ===========================================================================

bool Massmore_TCS34725::isConnected() {
  if (_wire == nullptr) return false;
  _wire->beginTransmission(_addr);
  return _wire->endTransmission() == 0;
}

bool Massmore_TCS34725::_writeAllConfig() {
  // ชิปไม่มี soft-reset → เขียนทุก register ที่ใช้ใหม่ทั้งหมดจาก shadow
  // เริ่มด้วย ENABLE ที่ไม่มี PON/AEN เพื่อให้ชิปหยุดก่อนเปลี่ยน config
  const uint8_t en = (uint8_t)((_wen ? MASSMORE_TCS34725_ENABLE_WEN : 0) |
                               (_aien ? MASSMORE_TCS34725_ENABLE_AIEN : 0));
  const uint8_t th[4] = { (uint8_t)(_ailt & 0xFF), (uint8_t)(_ailt >> 8),
                          (uint8_t)(_aiht & 0xFF), (uint8_t)(_aiht >> 8) };
  return writeRegister(MASSMORE_TCS34725_REG_ENABLE, en) &&
         writeRegister(MASSMORE_TCS34725_REG_ATIME, _atime) &&
         writeRegister(MASSMORE_TCS34725_REG_WTIME, _wtime) &&
         _writeBlock(MASSMORE_TCS34725_REG_AILTL, th, 4) &&
         writeRegister(MASSMORE_TCS34725_REG_PERS, (uint8_t)(_pers & MASSMORE_TCS34725_PERS_MASK)) &&
         writeRegister(MASSMORE_TCS34725_REG_CONFIG, _wlong ? MASSMORE_TCS34725_CONFIG_WLONG : 0) &&
         writeRegister(MASSMORE_TCS34725_REG_CONTROL, (uint8_t)_gain);
}

bool Massmore_TCS34725::begin(TwoWire &wirePort, uint8_t address) {
  _wire = &wirePort;
  _addr = address;
  _begun = false;
  _powered = false;
  _state = State::IDLE;
  _continuous = false;
  _newData = false;

  if (!isConnected()) { _err = ErrorCode::NOT_FOUND; return false; }
  if (!verifyChipID()) return false;                    // _err = WRONG_ID / BUS_ERROR
  if (!_writeAllConfig()) return false;

  _begun = true;
  powerOn();                                            // PON + warm-up 2.4 ms
  if (_err != ErrorCode::OK) { _begun = false; return false; }
  enableRGBC(true);                                     // เริ่ม integrate ทันที
  if (_err != ErrorCode::OK) { _begun = false; return false; }
  if (_ledViaInt) setLED(_ledOn);                       // ค่าเริ่มต้น: LED ดับ (หรือตามที่สั่งก่อน begin)
  return true;
}

void Massmore_TCS34725::powerOn() {
  if (!_ensureBegun()) return;
  if (!_updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_PON,
                   MASSMORE_TCS34725_ENABLE_PON)) return;
  // datasheet: ต้องรออย่างน้อย 2.4 ms หลัง PON ก่อนตั้ง AEN (เผื่อ 100 µs)
  delayMicroseconds(MASSMORE_TCS34725_WARMUP_US + 100u);
  _powered = true;
}

void Massmore_TCS34725::powerOff() {
  if (!_ensureBegun()) return;
  _updateBits(MASSMORE_TCS34725_REG_ENABLE,
              MASSMORE_TCS34725_ENABLE_PON | MASSMORE_TCS34725_ENABLE_AEN, 0);
  _powered = false;
  _continuous = false;
  _state = State::IDLE;
}

void Massmore_TCS34725::enableRGBC(bool on) {
  if (!_ensureBegun()) return;
  if (on && !_powered) { powerOn(); if (!_powered) return; }
  _updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_AEN,
              on ? MASSMORE_TCS34725_ENABLE_AEN : 0);
}

/// เริ่ม integration cycle ใหม่: AEN 1→0→1 (AVALID ถูกรีเซ็ต) — ข้อมูลรอบถัดไปจึงเป็นของใหม่แน่นอน
bool Massmore_TCS34725::_restartCycle() {
  if (!_powered) { powerOn(); if (!_powered) return false; }
  uint8_t en;
  if (!readRegister(MASSMORE_TCS34725_REG_ENABLE, en)) return false;
  en &= MASSMORE_TCS34725_ENABLE_MASK;
  return writeRegister(MASSMORE_TCS34725_REG_ENABLE, (uint8_t)(en & ~MASSMORE_TCS34725_ENABLE_AEN)) &&
         writeRegister(MASSMORE_TCS34725_REG_ENABLE,
                       (uint8_t)(en | MASSMORE_TCS34725_ENABLE_AEN | MASSMORE_TCS34725_ENABLE_PON));
}

/// poll STATUS.AVALID ทุก 1 ms จนครบ timeout (delay(1) ให้ RTOS / watchdog ทำงานได้)
bool Massmore_TCS34725::_waitValid(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  for (;;) {
    uint8_t st;
    if (!readRegister(MASSMORE_TCS34725_REG_STATUS, st)) return false;
    if (st & MASSMORE_TCS34725_STATUS_AVALID) return true;
    if ((uint32_t)(millis() - t0) >= timeoutMs) { _err = ErrorCode::TIMEOUT; return false; }
    delay(1);
  }
}

// ===========================================================================
//  Identity
// ===========================================================================

uint8_t Massmore_TCS34725::getChipID() {
  uint8_t id = 0;
  if (!readRegister(MASSMORE_TCS34725_REG_ID, id)) return 0;
  return id;
}

Massmore_TCS34725::Variant Massmore_TCS34725::getVariant() {
  if (_variant == Variant::UNKNOWN && _wire != nullptr) verifyChipID();
  return _variant;
}

bool Massmore_TCS34725::verifyChipID() {
  uint8_t id;
  if (!readRegister(MASSMORE_TCS34725_REG_ID, id)) { _variant = Variant::UNKNOWN; return false; }
  if (id == MASSMORE_TCS34725_ID_TCS34725)      _variant = Variant::TCS34725;
  else if (id == MASSMORE_TCS34725_ID_TCS34727) _variant = Variant::TCS34727;
  else { _variant = Variant::UNKNOWN; _err = ErrorCode::WRONG_ID; return false; }
  return true;
}

uint32_t Massmore_TCS34725::getSerialNumber() {
  // TCS3472 ไม่มี serial number / unique ID / silicon revision register (DS000390)
  return 0;
}

uint8_t Massmore_TCS34725::selfTestRegisters() {
  if (!_ensureBegun()) return 0;
  struct Pattern { uint8_t reg, val, mask; };
  static const Pattern kPat[9] = {
    { MASSMORE_TCS34725_REG_ATIME,   0xA5, 0xFF },
    { MASSMORE_TCS34725_REG_WTIME,   0x5A, 0xFF },
    { MASSMORE_TCS34725_REG_AILTL,   0x3C, 0xFF },
    { MASSMORE_TCS34725_REG_AILTH,   0xC3, 0xFF },
    { MASSMORE_TCS34725_REG_AIHTL,   0x96, 0xFF },
    { MASSMORE_TCS34725_REG_AIHTH,   0x69, 0xFF },
    { MASSMORE_TCS34725_REG_PERS,    0x0A, MASSMORE_TCS34725_PERS_MASK },
    { MASSMORE_TCS34725_REG_CONFIG,  0x02, MASSMORE_TCS34725_CONFIG_MASK },
    { MASSMORE_TCS34725_REG_CONTROL, 0x02, MASSMORE_TCS34725_CONTROL_AGAIN_MASK },
  };
  uint8_t enSaved = 0;
  readRegister(MASSMORE_TCS34725_REG_ENABLE, enSaved);

  uint8_t pass = 0;
  for (uint8_t i = 0; i < 9; i++) {
    uint8_t rb = 0;
    if (writeRegister(kPat[i].reg, kPat[i].val) && readRegister(kPat[i].reg, rb) &&
        (rb & kPat[i].mask) == (kPat[i].val & kPat[i].mask)) {
      pass++;
    }
  }
  // คืนค่า config ทั้งหมดจาก shadow แล้วคืน ENABLE เดิม (cycle ที่ค้างอยู่ถูกเริ่มใหม่)
  _writeAllConfig();
  writeRegister(MASSMORE_TCS34725_REG_ENABLE, (uint8_t)(enSaved & MASSMORE_TCS34725_ENABLE_MASK));
  _t0 = millis();
  return pass;
}

uint32_t Massmore_TCS34725::measureCycleTimeUs(uint8_t atime) {
  if (!_ensureBegun()) return 0;
  if (!_powered) { powerOn(); if (!_powered) return 0; }
  uint8_t enSaved = 0;
  if (!readRegister(MASSMORE_TCS34725_REG_ENABLE, enSaved)) return 0;

  const uint32_t cycles = 256u - atime;
  // timeout = 1.5 เท่าของค่าสูงสุดตาม datasheet + 20 ms
  const uint32_t limitUs = (1u + cycles) * MASSMORE_TCS34725_STEP_US_MAX * 3u / 2u + 20000u;
  uint32_t result = 0;

  // ปิด AEN / WEN / AIEN ก่อน เพื่อวัดเฉพาะ RGBC init + integration
  if (writeRegister(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_PON) &&
      writeRegister(MASSMORE_TCS34725_REG_ATIME, atime) &&
      writeRegister(MASSMORE_TCS34725_REG_ENABLE,
                    MASSMORE_TCS34725_ENABLE_PON | MASSMORE_TCS34725_ENABLE_AEN)) {
    const uint32_t t0 = micros();                 // STOP ของการเขียน AEN = จุดเริ่ม cycle
    for (;;) {
      uint8_t st;
      if (!readRegister(MASSMORE_TCS34725_REG_STATUS, st)) break;
      const uint32_t dt = (uint32_t)(micros() - t0);
      if (st & MASSMORE_TCS34725_STATUS_AVALID) { result = dt; break; }
      if (dt >= limitUs) { _err = ErrorCode::TIMEOUT; break; }
      // poll แบบไม่หน่วง — ความละเอียดของการวัด = เวลา 1 transaction (~0.1–0.4 ms)
    }
  }
  const ErrorCode e = _err;
  _writeAllConfig();
  writeRegister(MASSMORE_TCS34725_REG_ENABLE, (uint8_t)(enSaved & MASSMORE_TCS34725_ENABLE_MASK));
  _t0 = millis();
  if (result == 0) _err = (e == ErrorCode::OK) ? ErrorCode::TIMEOUT : e;
  return result;
}

bool Massmore_TCS34725::selfTestInterrupt() {
  if (!_ensureBegun()) return false;
  if (!_powered) { powerOn(); if (!_powered) return false; }
  uint8_t enSaved = 0;
  if (!readRegister(MASSMORE_TCS34725_REG_ENABLE, enSaved)) return false;

  const uint8_t PON  = MASSMORE_TCS34725_ENABLE_PON;
  const uint8_t AEN  = MASSMORE_TCS34725_ENABLE_AEN;
  const uint8_t AIEN = MASSMORE_TCS34725_ENABLE_AIEN;
  bool setOk = false, clearOk = false, quietOk = false;
  uint8_t st = 0;

  // ---- 1) threshold low = 0xFFFF → C < AILT เสมอ (1 cycle max count = 1024) → AINT ต้องตั้ง ----
  const uint8_t thOut[4] = { 0xFF, 0xFF, 0x00, 0x00 };
  if (writeRegister(MASSMORE_TCS34725_REG_ENABLE, PON) &&
      writeRegister(MASSMORE_TCS34725_REG_ATIME, 0xFF) &&                 // 1 cycle = 2.4 ms
      writeRegister(MASSMORE_TCS34725_REG_PERS, (uint8_t)Persistence::P1) &&
      _writeBlock(MASSMORE_TCS34725_REG_AILTL, thOut, 4) &&
      _sendCommand(MASSMORE_TCS34725_CMD_CLEAR_INT) &&
      writeRegister(MASSMORE_TCS34725_REG_ENABLE, PON | AEN | AIEN)) {
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 30u) {
      if (!readRegister(MASSMORE_TCS34725_REG_STATUS, st)) break;
      if (st & MASSMORE_TCS34725_STATUS_AINT) { setOk = true; break; }
      delay(1);
    }
  }

  // ---- 2) หยุด ADC แล้วส่ง 0xE6 → AINT ต้องหาย ----
  if (setOk &&
      writeRegister(MASSMORE_TCS34725_REG_ENABLE, PON | AIEN) &&
      _sendCommand(MASSMORE_TCS34725_CMD_CLEAR_INT) &&
      readRegister(MASSMORE_TCS34725_REG_STATUS, st)) {
    clearOk = (st & MASSMORE_TCS34725_STATUS_AINT) == 0;
  }

  // ---- 3) threshold low = 0, high = 0xFFFF → ไม่มีทางออกนอกช่วง → AINT ต้องไม่ตั้ง ----
  const uint8_t thIn[4] = { 0x00, 0x00, 0xFF, 0xFF };
  if (clearOk &&
      _writeBlock(MASSMORE_TCS34725_REG_AILTL, thIn, 4) &&
      writeRegister(MASSMORE_TCS34725_REG_ENABLE, PON | AEN | AIEN)) {
    delay(15);                                    // ≈ 3 cycles ของ ATIME 0xFF
    if (readRegister(MASSMORE_TCS34725_REG_STATUS, st)) {
      quietOk = (st & MASSMORE_TCS34725_STATUS_AVALID) && !(st & MASSMORE_TCS34725_STATUS_AINT);
    }
  }

  // ---- คืน config เดิม ----
  writeRegister(MASSMORE_TCS34725_REG_ENABLE, PON);
  _writeAllConfig();
  _sendCommand(MASSMORE_TCS34725_CMD_CLEAR_INT);
  writeRegister(MASSMORE_TCS34725_REG_ENABLE, (uint8_t)(enSaved & MASSMORE_TCS34725_ENABLE_MASK));
  _t0 = millis();
  return setOk && clearOk && quietOk;
}

bool Massmore_TCS34725::isGenuine() {
  if (!_ensureBegun()) return false;
  // 1) ID ตาม datasheet (ID อื่นเช่น 0x10 ไม่มีใน datasheet → ไม่ใช่ของแท้)
  if (!verifyChipID()) return false;
  // 2) register R/W ครบ 9 ตัว
  if (selfTestRegisters() != 9) return false;
  // 3) INIT timing: ATIME 0xF6 = 10 cycles → AVALID หลัง 11 step × 2.27–2.56 ms
  //    เผื่อ −0.5 ms (micros jitter) / +1.5 ms (เวลา poll I2C ที่ 100 kHz)
  //    วัด 3 ครั้งแล้วใช้ค่าต่ำสุด — การเริ่ม AEN ใหม่มี latency สุ่มได้ถึง ~1 step (ทดสอบกับบอร์ดจริง)
  uint32_t t = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < 3; i++) {
    const uint32_t m = measureCycleTimeUs(0xF6);
    if (m == 0) return false;
    if (m < t) t = m;
  }
  const uint32_t tMin = 11u * MASSMORE_TCS34725_STEP_US_MIN - 500u;
  const uint32_t tMax = 11u * MASSMORE_TCS34725_STEP_US_MAX + 1500u;
  return t >= tMin && t <= tMax;
}

// ===========================================================================
//  Configuration
// ===========================================================================

bool Massmore_TCS34725::setIntegrationTime(float ms) {
  if (!(ms >= 1.2f && ms <= 615.6f)) { _err = ErrorCode::INVALID_ARG; return false; }
  long cycles = (long)(ms / 2.4f + 0.5f);
  if (cycles < 1) cycles = 1;
  if (cycles > 256) cycles = 256;
  return setIntegrationCycles((uint16_t)cycles);
}

bool Massmore_TCS34725::setIntegrationCycles(uint16_t cycles) {
  if (cycles < 1 || cycles > 256) { _err = ErrorCode::INVALID_ARG; return false; }
  _atime = (uint8_t)(256u - cycles);
  if (!_begun) return true;                      // ยังไม่ begin: เก็บใน shadow แล้วเขียนตอน begin()
  return writeRegister(MASSMORE_TCS34725_REG_ATIME, _atime);
}

void Massmore_TCS34725::setATIME(uint8_t raw) {
  _atime = raw;
  if (_begun) writeRegister(MASSMORE_TCS34725_REG_ATIME, _atime);
}

float Massmore_TCS34725::getIntegrationTimeMs() const {
  return (float)getIntegrationCycles() * 2.4f;
}

uint16_t Massmore_TCS34725::getMaxCount() const {
  const uint32_t c = getIntegrationCycles();
  return (c >= 64u) ? 65535u : (uint16_t)(1024u * c);
}

void Massmore_TCS34725::setGain(Gain g) {
  _gain = g;
  if (_begun) _updateBits(MASSMORE_TCS34725_REG_CONTROL, MASSMORE_TCS34725_CONTROL_AGAIN_MASK, (uint8_t)g);
}

float Massmore_TCS34725::getGainValue() const {
  return kGainNominal[(uint8_t)_gain & 0x03];
}

bool Massmore_TCS34725::setWaitTime(float ms) {
  if (ms != ms || ms < 0.0f || ms > 7372.8f + 14.4f) { _err = ErrorCode::INVALID_ARG; return false; }
  if (ms == 0.0f) {
    _wen = false;
  } else {
    long steps = (long)(ms / 2.4f + 0.5f);
    if (steps < 1) steps = 1;
    if (steps <= 256) {
      _wlong = false;
    } else {
      steps = (long)(ms / (2.4f * MASSMORE_TCS34725_WLONG_FACTOR) + 0.5f);   // 28.8 ms ต่อ step
      if (steps < 1) steps = 1;
      if (steps > 256) steps = 256;
      _wlong = true;
    }
    _wtime = (uint8_t)(256 - steps);
    _wen = true;
  }
  if (!_begun) return true;
  return writeRegister(MASSMORE_TCS34725_REG_WTIME, _wtime) &&
         _updateBits(MASSMORE_TCS34725_REG_CONFIG, MASSMORE_TCS34725_CONFIG_WLONG,
                     _wlong ? MASSMORE_TCS34725_CONFIG_WLONG : 0) &&
         _updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_WEN,
                     _wen ? MASSMORE_TCS34725_ENABLE_WEN : 0);
}

float Massmore_TCS34725::getWaitTimeMs() const {
  if (!_wen) return 0.0f;
  return (float)(256u - _wtime) * 2.4f * (_wlong ? (float)MASSMORE_TCS34725_WLONG_FACTOR : 1.0f);
}

void Massmore_TCS34725::setPersistence(Persistence p) {
  _pers = (uint8_t)p & MASSMORE_TCS34725_PERS_MASK;
  if (_begun) _updateBits(MASSMORE_TCS34725_REG_PERS, MASSMORE_TCS34725_PERS_MASK, _pers);
}

void Massmore_TCS34725::setInterruptThresholds(uint16_t low, uint16_t high) {
  _ailt = low;
  _aiht = high;
  if (!_begun) return;
  const uint8_t th[4] = { (uint8_t)(low & 0xFF), (uint8_t)(low >> 8),
                          (uint8_t)(high & 0xFF), (uint8_t)(high >> 8) };
  _writeBlock(MASSMORE_TCS34725_REG_AILTL, th, 4);
}

void Massmore_TCS34725::enableInterrupt(bool on) {
  _aien = on;
  if (_begun) _updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_AIEN,
                          on ? MASSMORE_TCS34725_ENABLE_AIEN : 0);
}

void Massmore_TCS34725::clearInterrupt() {
  _sendCommand(MASSMORE_TCS34725_CMD_CLEAR_INT);
}

uint8_t Massmore_TCS34725::getStatus() {
  uint8_t st = 0;
  readRegister(MASSMORE_TCS34725_REG_STATUS, st);
  return st;
}

bool Massmore_TCS34725::isInterruptFlag() { return (getStatus() & MASSMORE_TCS34725_STATUS_AINT) != 0; }
bool Massmore_TCS34725::isValid()         { return (getStatus() & MASSMORE_TCS34725_STATUS_AVALID) != 0; }

// ===========================================================================
//  Simple Blocking API
// ===========================================================================

bool Massmore_TCS34725::readRaw(RawData &out) {
  if (!_ensureBegun()) return false;
  if (!_restartCycle()) return false;
  if (!_waitValid(_timeoutMs())) return false;
  if (!_readData(out)) return false;
  if (_continuous) { _restartCycle(); _t0 = millis(); }   // ไม่ให้ FSM ได้ข้อมูลรอบเดียวกันซ้ำ
  return true;
}

bool Massmore_TCS34725::readLatestRaw(RawData &out) {
  if (!_ensureBegun()) return false;
  uint8_t st;
  if (!readRegister(MASSMORE_TCS34725_REG_STATUS, st)) return false;
  if (!(st & MASSMORE_TCS34725_STATUS_AVALID)) { _err = ErrorCode::NOT_READY; return false; }
  return _readData(out);
}

bool Massmore_TCS34725::readRawOneShot(RawData &out) {
  if (!_ensureBegun()) return false;
  const bool ok = readRaw(out);
  const ErrorCode e = _err;
  powerOff();
  if (!ok) _err = e;
  return ok;
}

uint16_t Massmore_TCS34725::readClear() { RawData d; return readRaw(d) ? d.c : 0; }
uint16_t Massmore_TCS34725::readRed()   { RawData d; return readRaw(d) ? d.r : 0; }
uint16_t Massmore_TCS34725::readGreen() { RawData d; return readRaw(d) ? d.g : 0; }
uint16_t Massmore_TCS34725::readBlue()  { RawData d; return readRaw(d) ? d.b : 0; }

bool Massmore_TCS34725::readRGB8(RGB8 &out, bool gamma) {
  RawData d;
  if (!readRaw(d)) return false;
  if (_hasWhite) out = toRGB8(applyCalibration(d), gamma, MASSMORE_TCS34725_CAL_SCALE);
  else           out = toRGB8(applyCalibration(d), gamma, 0);
  return true;
}

float Massmore_TCS34725::readLux() {
  RawData d;
  if (!readRaw(d)) return NAN;
  if (isSaturated(d)) { _err = ErrorCode::SATURATED; return NAN; }
  return calculateLux(d);
}

float Massmore_TCS34725::readColorTemperature() {
  RawData d;
  if (!readRaw(d)) return NAN;
  if (isSaturated(d)) { _err = ErrorCode::SATURATED; return NAN; }
  return calculateColorTemperature(d);
}

// ===========================================================================
//  Non-blocking FSM
// ===========================================================================

bool Massmore_TCS34725::startContinuous() {
  if (!_ensureBegun()) return false;
  _continuous = true;
  _newData = false;
  if (!_restartCycle()) { _state = State::ERROR; return false; }
  _t0 = millis();
  _state = State::INTEGRATING;
  return true;
}

void Massmore_TCS34725::stopContinuous() {
  _continuous = false;
  _state = State::IDLE;
  if (_begun) _updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_AEN, 0);
}

bool Massmore_TCS34725::requestConversion() {
  if (!_ensureBegun()) return false;
  _continuous = false;
  _newData = false;
  if (!_powered) {
    // ตั้ง PON แล้วให้ update() รอ warm-up เป็น state (ไม่ใช้ delay)
    if (!_updateBits(MASSMORE_TCS34725_REG_ENABLE,
                     MASSMORE_TCS34725_ENABLE_PON | MASSMORE_TCS34725_ENABLE_AEN,
                     MASSMORE_TCS34725_ENABLE_PON)) { _state = State::ERROR; return false; }
    _t0 = micros();
    _state = State::WARMUP;
    return true;
  }
  if (!_restartCycle()) { _state = State::ERROR; return false; }
  _t0 = millis();
  _state = State::INTEGRATING;
  return true;
}

void Massmore_TCS34725::update() {
  switch (_state) {
    case State::WARMUP:
      if ((uint32_t)(micros() - _t0) >= MASSMORE_TCS34725_WARMUP_US + 100u) {
        _powered = true;
        if (_updateBits(MASSMORE_TCS34725_REG_ENABLE, MASSMORE_TCS34725_ENABLE_AEN,
                        MASSMORE_TCS34725_ENABLE_AEN)) {
          _t0 = millis();
          _state = State::INTEGRATING;
        } else {
          _state = State::ERROR;
        }
      }
      break;

    case State::INTEGRATING: {
      const uint32_t elapsed = (uint32_t)(millis() - _t0);
      if (elapsed < _minCycleMs()) return;          // ยังไม่ถึงเวลา — ไม่แตะ I2C bus
      uint8_t st;
      const bool ok = readRegister(MASSMORE_TCS34725_REG_STATUS, st);
      if (ok && (st & MASSMORE_TCS34725_STATUS_AVALID)) {
        if (_readData(_data)) {
          _newData = true;
          if (_continuous) {
            _restartCycle();                        // รอบใหม่ → ไม่มีวันได้ข้อมูลซ้ำ
            _t0 = millis();
          } else {
            _updateBits(MASSMORE_TCS34725_REG_ENABLE,
                        MASSMORE_TCS34725_ENABLE_PON | MASSMORE_TCS34725_ENABLE_AEN, 0);
            _powered = false;
            _state = State::READY;
          }
        }
        return;
      }
      if (elapsed >= _timeoutMs()) {
        _err = ok ? ErrorCode::TIMEOUT : ErrorCode::BUS_ERROR;
        if (_continuous) {                          // ลองใหม่อัตโนมัติ
          _restartCycle();
          _t0 = millis();
        } else {
          _state = State::ERROR;
        }
      }
      break;
    }

    default:
      break;
  }
}

bool Massmore_TCS34725::isDataReady() {
  if (!_newData) return false;
  _newData = false;
  return true;
}

// ===========================================================================
//  Auto-range
// ===========================================================================

int8_t Massmore_TCS34725::_findRangeIndex() const {
  const float cur = _sensitivity();
  int8_t best = 0;
  for (uint8_t i = 0; i < kRangeCount; i++) {
    const float s = (float)kRangeCycles[i] * kGainNominal[kRangeGain[i]];
    if (s <= cur + 0.01f) best = (int8_t)i;
  }
  return best;
}

void Massmore_TCS34725::_applyRange(uint8_t idx) {
  setIntegrationCycles(kRangeCycles[idx]);
  setGain((Gain)kRangeGain[idx]);
}

bool Massmore_TCS34725::_rangeAdjust(const RawData &d) {
  const int8_t idx = _findRangeIndex();
  const float  cur = _sensitivity();
  const float  idxSens = (float)kRangeCycles[idx] * kGainNominal[kRangeGain[idx]];
  const bool   onLadder = (idxSens > cur - 0.01f);
  const uint32_t high = (uint32_t)getMaxCount() * _arMaxPct / 100u;

  // ---- สว่างเกิน → ลดความไว ----
  if (isSaturated(d) || d.c >= high) {
    if (!onLadder) { _applyRange((uint8_t)idx); return true; }
    if (idx > 0)   { _applyRange((uint8_t)(idx - 1)); return true; }
    return false;                                   // ต่ำสุดแล้ว
  }

  // ---- มืดเกิน → เพิ่มความไว (ถ้าค่าที่คาดการณ์ยังไม่ชนเพดาน × 0.8 กันแกว่ง) ----
  if (_arMinClear > 0 && d.c >= _arMinClear) return false;
  const uint8_t next = (uint8_t)(idx + 1);
  if (next >= kRangeCount) return false;
  const float nextSens = (float)kRangeCycles[next] * kGainNominal[kRangeGain[next]];
  const float predicted = (float)d.c * nextSens / cur;
  const uint32_t nextMax = (kRangeCycles[next] >= 64u) ? 65535u : 1024u * kRangeCycles[next];
  if (predicted < (float)nextMax * _arMaxPct / 100.0f * 0.8f) {
    _applyRange(next);
    return true;
  }
  return false;
}

bool Massmore_TCS34725::autoRangeStep() {
  if (!_ensureBegun()) return false;
  if (!_rangeAdjust(_data)) return false;
  _newData = false;
  if (_state == State::INTEGRATING) {               // รอบที่ค้างใช้ค่าเก่า → เริ่มใหม่
    _restartCycle();
    _t0 = millis();
  }
  return true;
}

bool Massmore_TCS34725::autoRange(uint16_t targetMinClear, uint8_t targetMaxPercent) {
  if (targetMaxPercent < 1 || targetMaxPercent > 100) { _err = ErrorCode::INVALID_ARG; return false; }
  _arMinClear = targetMinClear;
  _arMaxPct = targetMaxPercent;
  RawData d;
  for (uint8_t i = 0; i < 8; i++) {
    if (!readRaw(d)) return false;
    if (!_rangeAdjust(d)) {
      _data = d;
      if (isSaturated(d)) { _err = ErrorCode::SATURATED; return false; }
      return true;
    }
  }
  if (!readRaw(d)) return false;
  _data = d;
  if (isSaturated(d)) { _err = ErrorCode::SATURATED; return false; }
  return true;
}

// ===========================================================================
//  LED
// ===========================================================================

void Massmore_TCS34725::attachLED(int8_t pin, bool activeHigh) {
  if (_ledViaInt) detachLED();                      // คืนขา INT ก่อนย้ายไป GPIO
  _ledPin = pin;
  _ledActiveHigh = activeHigh;
  if (_ledPin < 0) return;
  pinMode(_ledPin, OUTPUT);
  setLED(false);
}

void Massmore_TCS34725::attachLEDToINT() {
  _ledPin = -1;
  _ledViaInt = true;
  if (_begun) setLED(_ledOn);                       // ใช้สถานะล่าสุดที่สั่ง
}

void Massmore_TCS34725::detachLED() {
  if (_ledViaInt && _begun) {
    enableInterrupt(false);                         // ปล่อยขา INT
    setInterruptThresholds(0x0000, 0xFFFF);         // คืน threshold ค่าเริ่มต้น
    clearInterrupt();
  }
  _ledViaInt = false;
  _ledPin = -1;
  _ledOn = false;
}

void Massmore_TCS34725::setLED(bool on) {
  _ledOn = on;
  if (_ledViaInt) {
    if (!_begun) return;                            // จำสถานะไว้ ไปมีผลตอน begin()
    if (on) {
      enableInterrupt(false);                       // ปล่อยขา INT → pull-up → LED ติดทันที
      clearInterrupt();
    } else {
      // low = 0xFFFF / high = 0 → C < AILT ทุกครั้ง, APERS = EVERY → AINT ตั้งทุก cycle
      setInterruptThresholds(0xFFFF, 0x0000);
      setPersistence(Persistence::EVERY);
      enableInterrupt(true);                        // INT ดึงลงเมื่อ AINT ตั้ง → LED ดับ
      uint8_t en = 0;
      if (readRegister(MASSMORE_TCS34725_REG_ENABLE, en) && !(en & MASSMORE_TCS34725_ENABLE_AEN)) {
        enableRGBC(true);                           // ต้องมี RGBC cycle ถึงจะเกิด AINT
      }
    }
    return;
  }
  if (_ledPin < 0) return;
  digitalWrite(_ledPin, (on == _ledActiveHigh) ? HIGH : LOW);
}

// ===========================================================================
//  Diagnostics
// ===========================================================================

const char *Massmore_TCS34725::errorToString(ErrorCode e) {
  switch (e) {
    case ErrorCode::OK:          return "OK";
    case ErrorCode::NOT_FOUND:   return "NOT_FOUND";
    case ErrorCode::WRONG_ID:    return "WRONG_ID";
    case ErrorCode::TIMEOUT:     return "TIMEOUT";
    case ErrorCode::BUS_ERROR:   return "BUS_ERROR";
    case ErrorCode::NOT_READY:   return "NOT_READY";
    case ErrorCode::INVALID_ARG: return "INVALID_ARG";
    case ErrorCode::SATURATED:   return "SATURATED";
  }
  return "UNKNOWN";
}

static void printHex2(Print &out, uint8_t v) {
  if (v < 0x10) out.print('0');
  out.print(v, HEX);
}

void Massmore_TCS34725::dumpRegisters(Print &out) {
  static const uint8_t kRegs[] = { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0F,
                                   0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };
  for (uint8_t i = 0; i < sizeof(kRegs); i++) {
    uint8_t v = 0;
    const bool ok = readRegister(kRegs[i], v);
    out.print(F("0x"));
    printHex2(out, kRegs[i]);
    out.print(F(" = "));
    if (ok) { out.print(F("0x")); printHex2(out, v); }
    else    { out.print(F("ERR")); }
    out.println();
  }
}
