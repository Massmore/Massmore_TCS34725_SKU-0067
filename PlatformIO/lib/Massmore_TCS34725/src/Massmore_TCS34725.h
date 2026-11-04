/**
 * @file    Massmore_TCS34725.h
 * @brief   Driver สำหรับเซ็นเซอร์สี RGBC ams-OSRAM TCS34725 / TCS34727 (Massmore SKU-0067)
 *
 * จุดเด่น
 *  - ใช้แค่ Wire ไม่มี dependency ภายนอก ไม่มี dynamic allocation (ไม่มี new / malloc / String)
 *  - อ่าน C,R,G,B แบบ block 8 ไบต์ใน I2C transaction เดียว → ทุกช่องมาจาก integration cycle เดียวกัน
 *  - Dual API: Simple Blocking (readRaw / readLux …) และ Non-blocking FSM (update / isDataReady …)
 *  - Lux / CCT ตาม ams DN40 (IR rejection, Glass Attenuation, Device Factor) + ตรวจ saturation
 *  - Auto-range (gain + integration time), dark / white calibration, interrupt + low-power wait
 *  - ไลบรารีไม่เรียก Wire.begin() — sketch เป็นเจ้าของ I2C bus เสมอ (กำหนดขา / clock เอง)
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 * SPDX-License-Identifier: MIT
 */
#ifndef MASSMORE_TCS34725_H
#define MASSMORE_TCS34725_H

#include <Arduino.h>
#include <Wire.h>
#include "Massmore_TCS34725_Registers.h"

#define MASSMORE_TCS34725_VERSION       "1.0.0"
#define MASSMORE_TCS34725_ADDR_DEFAULT  0x29   ///< TCS34725 / TCS34727 (I2C address คงที่)
#define MASSMORE_TCS34725_ADDR_ALT      0x39   ///< TCS34721 / TCS34723 (รุ่นที่ address ต่างกัน)
#define MASSMORE_TCS34725_CAL_SCALE     10000u ///< applyCalibration(): white reference = 10000

class Massmore_TCS34725 {
public:
  // ======================================================================
  //  Types
  // ======================================================================

  /// รหัสข้อผิดพลาด ดูด้วย lastError() / errorToString()
  enum class ErrorCode : uint8_t {
    OK = 0,      ///< สำเร็จ
    NOT_FOUND,   ///< ไม่มี ACK จาก address (สาย / ไฟ / pull-up)
    WRONG_ID,    ///< ID register ไม่ใช่ 0x44 / 0x4D
    TIMEOUT,     ///< รอ AVALID เกินเวลาที่คำนวณจาก datasheet
    BUS_ERROR,   ///< I2C NACK หรืออ่านได้ไม่ครบไบต์
    NOT_READY,   ///< ยังไม่ได้ begin() หรือข้อมูลยังไม่พร้อม
    INVALID_ARG, ///< parameter อยู่นอกช่วงที่ชิปรองรับ
    SATURATED    ///< clear channel อิ่มตัว — Lux / CCT ไม่น่าเชื่อถือ
  };

  /// ชิปที่ตรวจพบจาก ID register
  enum class Variant : uint8_t { UNKNOWN = 0, TCS34725, TCS34727 };

  /// Analog gain (AGAIN) — ค่าจริงตาม datasheet: 4× = 3.8–4.2, 16× = 15–16.8, 60× = 58–63
  enum class Gain : uint8_t { X1 = 0, X4 = 1, X16 = 2, X60 = 3 };

  /// Interrupt persistence (APERS) — จำนวนรอบติดกันที่ค่าอยู่นอก threshold ก่อนเกิด interrupt
  enum class Persistence : uint8_t {
    EVERY = 0, ///< ทุก RGBC cycle เกิด interrupt (ไม่สน threshold)
    P1, P2, P3, P5, P10, P15, P20, P25, P30, P35, P40, P45, P50, P55, P60
  };

  /// สถานะของ Non-blocking FSM
  enum class State : uint8_t {
    IDLE,        ///< ไม่ได้วัด (หรือ one-shot อ่านไปแล้ว)
    WARMUP,      ///< ตั้ง PON แล้ว รอ 2.4 ms ก่อนตั้ง AEN
    INTEGRATING, ///< ADC กำลัง integrate รอ AVALID
    READY,       ///< One-shot เสร็จ มีข้อมูลใหม่ (ชิปกลับไป sleep แล้ว)
    ERROR        ///< One-shot ล้มเหลว (ดู lastError())
  };

  /// วิธีคำนวณ Correlated Color Temperature
  enum class CctMethod : uint8_t { DN40, MCCAMY };

  /// ค่าดิบ 16-bit ทั้ง 4 ช่อง (Clear, Red, Green, Blue)
  struct RawData { uint16_t c, r, g, b; };
  /// สี 8-bit ต่อช่อง (เช่นส่งต่อให้ RGB LED / แสดงเป็น HEX)
  struct RGB8 { uint8_t r, g, b; };
  /// สีแบบ HSV — h 0–360 องศา, s / v 0–1
  struct HSV { float h, s, v; };
  /// ผลรวมด้านแสง (DN40)
  struct LightData {
    float lux;       ///< ความสว่าง (lux) หรือ NAN
    float cct;       ///< อุณหภูมิสี (Kelvin) หรือ NAN
    float ir;        ///< ค่า IR โดยประมาณ (counts)
    bool  saturated; ///< true = อิ่มตัว (analog / digital / ripple)
    bool  valid;     ///< true = lux และ cct ใช้งานได้
  };

  Massmore_TCS34725();

  // ======================================================================
  //  Lifecycle
  // ======================================================================

  /**
   * @brief  เริ่มต้นเซ็นเซอร์: ตรวจ ACK + ID, เขียน config ทุก register ใหม่, Power ON และเริ่มวัด
   * @note   ไม่เรียก Wire.begin() — ต้องเรียก Wire.begin() ใน sketch ก่อนเสมอ
   *         ชิปไม่มี soft-reset จึงต้องเขียน config ทุกตัวใหม่ทุกครั้ง (MCU รีเซ็ตแต่ชิปอาจค้างค่าเดิม)
   * @param  wirePort  I2C bus ที่ใช้ (Wire / Wire1)
   * @param  address   I2C address (0x29 ค่าเริ่มต้น, 0x39 สำหรับ TCS34721/23)
   * @return true ถ้าพบชิปที่รู้จัก (0x44 / 0x4D) และตั้งค่าสำเร็จ
   */
  bool begin(TwoWire &wirePort = Wire, uint8_t address = MASSMORE_TCS34725_ADDR_DEFAULT);

  /** @brief  ตรวจว่ามี ACK ที่ address หรือไม่ (ไม่อ่าน ID) */
  bool isConnected();

  /** @brief  ตั้ง PON และรอ warm-up 2.4 ms (delayMicroseconds ครั้งเดียว) */
  void powerOn();

  /** @brief  เข้า Sleep (ปิด PON และ AEN) — กินไฟประมาณ 2.5 µA */
  void powerOff();

  /** @brief  เปิด / ปิด RGBC ADC (AEN) — ถ้ายังไม่ได้ powerOn() จะ powerOn ให้ก่อน */
  void enableRGBC(bool on = true);

  // ======================================================================
  //  Identity / Authenticity
  // ======================================================================

  /** @brief  อ่าน ID register (0x12) — คืน 0 ถ้าอ่านไม่ได้ */
  uint8_t getChipID();

  /** @brief  รุ่นชิปที่ตรวจพบตอน begin() (หรืออ่านใหม่ถ้ายังไม่ได้ begin) */
  Variant getVariant();

  /** @brief  อ่าน ID แล้วเทียบกับค่าใน datasheet (0x44 = TCS34725, 0x4D = TCS34727) */
  bool verifyChipID();

  /**
   * @brief  Serial number ของชิป
   * @return 0 เสมอ — TCS3472 ไม่มี serial number / unique ID / revision register ตาม datasheet
   */
  uint32_t getSerialNumber();

  /**
   * @brief  ตรวจของแท้แบบ heuristic: ID + register R/W + INIT timing (ไม่ต้องใช้แสงภายนอก)
   * @note   ใช้เวลาประมาณ 100 ms และคืน config เดิมให้ทั้งหมดเมื่อเสร็จ
   *         ไม่สามารถยืนยันของแท้ได้ 100% เพราะชิปไม่มี unique ID / trim register
   */
  bool isGenuine();

  /**
   * @brief  ทดสอบเขียน/อ่านกลับ 9 register (ATIME, WTIME, AILT/AIHT ×4, PERS, CONFIG, CONTROL)
   * @return จำนวน register ที่ผ่าน (เต็ม = 9) — คืนค่าเดิมทุก register หลังทดสอบ
   */
  uint8_t selfTestRegisters();

  /**
   * @brief  วัดเวลาตั้งแต่ตั้ง AEN จนได้ AVALID ที่ ATIME ที่กำหนด (หน่วย µs)
   * @note   ค่าที่ควรได้ = (1 + cycles) × 2.27–2.56 ms (+ เวลา poll I2C) — ใช้ตรวจ oscillator ของชิป
   * @param  atime  ค่า ATIME สำหรับทดสอบ (ค่าเริ่มต้น 0xF6 = 10 cycles)
   * @return เวลาเป็น µs หรือ 0 ถ้า timeout / bus error (คืน config เดิมเมื่อเสร็จ)
   */
  uint32_t measureCycleTimeUs(uint8_t atime = 0xF6);

  /**
   * @brief  ทดสอบ interrupt logic: AINT ต้องตั้งเมื่อค่าอยู่นอก threshold, ต้องไม่ตั้งเมื่ออยู่ใน
   *         threshold และต้องหายหลังส่ง special function 0xE6 (คืน config เดิมเมื่อเสร็จ)
   */
  bool selfTestInterrupt();

  // ======================================================================
  //  Configuration
  // ======================================================================

  /**
   * @brief  ตั้ง integration time เป็น ms (ปัดเป็นขั้นละ 2.4 ms)
   * @param  ms  2.4 – 614.4 ms
   * @return false ถ้าอยู่นอกช่วง (INVALID_ARG) หรือเขียนไม่สำเร็จ
   */
  bool setIntegrationTime(float ms);

  /** @brief  ตั้ง integration เป็นจำนวน cycle 1–256 (1 cycle = 2.4 ms, max count = 1024/cycle) */
  bool setIntegrationCycles(uint16_t cycles);

  /** @brief  เขียน ATIME ดิบ (0xFF = 1 cycle … 0x00 = 256 cycles) */
  void setATIME(uint8_t raw);

  /** @brief  ATIME ปัจจุบัน (ค่าดิบ) */
  uint8_t getATIME() const { return _atime; }

  /** @brief  จำนวน integration cycle ปัจจุบัน (1–256) */
  uint16_t getIntegrationCycles() const { return (uint16_t)(256u - _atime); }

  /** @brief  integration time ปัจจุบัน (ms, ค่า typ.) */
  float getIntegrationTimeMs() const;

  /** @brief  ค่าสูงสุดที่ ADC นับได้ = min(65535, 1024 × cycles) */
  uint16_t getMaxCount() const;

  /** @brief  ตั้ง analog gain */
  void setGain(Gain g);
  /** @brief  gain ปัจจุบัน */
  Gain getGain() const { return _gain; }
  /** @brief  gain เป็นตัวเลข (1, 4, 16, 60 — ค่า nominal) */
  float getGainValue() const;

  /**
   * @brief  ตั้ง wait time ระหว่างรอบวัด (โหมดประหยัดพลังงาน ใช้คู่กับ interrupt)
   * @param  ms  0 = ปิด wait (WEN = 0), 2.4–614.4 ms ปกติ, >614.4 ms ตั้ง WLONG อัตโนมัติ (สูงสุด 7372.8 ms)
   * @return false ถ้าเกินช่วง
   */
  bool setWaitTime(float ms);
  /** @brief  wait time ปัจจุบัน (ms) — 0 ถ้าปิด WEN */
  float getWaitTimeMs() const;

  // ⚠ ค่าเริ่มต้นไลบรารีใช้ขา INT คุม LED บนบอร์ด — จะใช้ interrupt เอง เรียก detachLED() ก่อน (ดูตัวอย่าง 06)

  /** @brief  ตั้ง interrupt persistence (APERS) */
  void setPersistence(Persistence p);

  /**
   * @brief  ตั้ง clear-channel threshold — interrupt เกิดเมื่อ C < low หรือ C > high
   * @note   เขียนแบบ auto-increment 4 ไบต์ใน transaction เดียว
   */
  void setInterruptThresholds(uint16_t low, uint16_t high);

  /** @brief  เปิด / ปิด ขา INT (AIEN) — ขา INT เป็น open-drain active-low ต้องมี pull-up */
  void enableInterrupt(bool on = true);

  /** @brief  Clear interrupt ด้วย special function 0xE6 (ขา INT กลับเป็น HIGH) */
  void clearInterrupt();

  /** @brief  อ่าน STATUS register (0x13) */
  uint8_t getStatus();
  /** @brief  STATUS.AINT — มี interrupt ค้างอยู่ */
  bool isInterruptFlag();
  /** @brief  STATUS.AVALID — มีผลวัดอย่างน้อย 1 รอบหลังตั้ง AEN */
  bool isValid();

  // ======================================================================
  //  Simple Blocking API
  // ======================================================================

  /**
   * @brief  เริ่ม integration cycle ใหม่ รอ AVALID (มี timeout) แล้วอ่าน C,R,G,B แบบ block 8 ไบต์
   * @note   รับประกันว่าได้ข้อมูลใหม่ทุกครั้ง ใช้เวลา ≈ 2.4 ms + integration time
   * @return false ถ้า timeout / bus error (ดู lastError())
   */
  bool readRaw(RawData &out);

  /**
   * @brief  อ่านข้อมูลล่าสุดใน data register ทันที ไม่เริ่มรอบใหม่ ไม่รอ
   * @note   เหมาะกับโหมด interrupt / wait ที่ชิปวัดเองเป็นรอบ ๆ
   * @return false ถ้ายังไม่มี AVALID (NOT_READY) หรือ bus error
   */
  bool readLatestRaw(RawData &out);

  /** @brief  Power ON → วัด 1 รอบ → Sleep (ประหยัดพลังงาน) */
  bool readRawOneShot(RawData &out);

  /** @brief  อ่านเฉพาะช่อง (เรียก readRaw ภายใน) — คืน 0 ถ้าอ่านไม่ได้ */
  uint16_t readClear();
  uint16_t readRed();   ///< @copydoc readClear
  uint16_t readGreen(); ///< @copydoc readClear
  uint16_t readBlue();  ///< @copydoc readClear

  /**
   * @brief  อ่านสีเป็น 8-bit — ถ้ามี white reference แล้วจะใช้ค่าที่ calibrate (สะท้อนความสว่างจริง)
   * @param  gamma  true = ใส่ gamma 2.5 สำหรับส่งให้ RGB LED
   */
  bool readRGB8(RGB8 &out, bool gamma = false);

  /** @brief  อ่าน lux (DN40) — NAN ถ้า saturated หรืออ่านไม่ได้ */
  float readLux();
  /** @brief  อ่านอุณหภูมิสี (K, DN40) — NAN ถ้า saturated หรืออ่านไม่ได้ */
  float readColorTemperature();

  // ======================================================================
  //  Non-blocking FSM — เรียก update() ทุกรอบ loop()
  // ======================================================================

  /**
   * @brief  เริ่มวัดต่อเนื่องแบบไม่ block — update() จะอ่านข้อมูลและเริ่มรอบใหม่ให้อัตโนมัติ
   * @note   ทุกรอบเริ่ม cycle ใหม่หลังอ่าน จึงรับประกันว่าแต่ละผลเป็นข้อมูลใหม่ (ไม่ซ้ำรอบเดิม)
   */
  bool startContinuous();

  /** @brief  หยุดโหมดต่อเนื่อง (ปิด AEN) — ชิปยัง Power ON อยู่ */
  void stopContinuous();

  /**
   * @brief  สั่งวัด 1 รอบแบบไม่ block (warm-up เป็น state ไม่ใช้ delay) เสร็จแล้วชิปกลับ Sleep
   * @return false ถ้า bus error
   */
  bool requestConversion();

  /** @brief  ขับ FSM — ใช้ millis() แบบ rollover-safe, poll STATUS เฉพาะเมื่อถึงเวลาที่ควรเสร็จ */
  void update();

  /** @brief  true ครั้งเดียวต่อผลวัดใหม่ (อ่านแล้ว flag ถูก clear) */
  bool isDataReady();

  /** @brief  ผลวัดล่าสุดจาก FSM */
  const RawData &getReadings() const { return _data; }

  /** @brief  สถานะ FSM ปัจจุบัน */
  State getState() const { return _state; }

  /**
   * @brief  ปรับ gain / ATIME 1 ขั้นจากผลวัดล่าสุด (ใช้หลัง isDataReady())
   * @return true ถ้ามีการเปลี่ยนค่า (ผลรอบถัดไปจะเป็นค่าใหม่ — รอบที่วัดค้างอยู่ถูกเริ่มใหม่)
   */
  bool autoRangeStep();

  // ======================================================================
  //  Auto-range (Blocking)
  // ======================================================================

  /**
   * @brief  หา gain + integration time ที่เหมาะสม (blocking สูงสุด 8 รอบวัด)
   * @param  targetMinClear    clear count ขั้นต่ำที่ "พอแล้ว" (0 = เพิ่มความไวให้มากที่สุดที่ไม่อิ่มตัว)
   * @param  targetMaxPercent  เพดาน % ของ max count ก่อนลดความไว (1–100, แนะนำ 75)
   * @return true ถ้าได้ช่วงที่ไม่ saturated
   */
  bool autoRange(uint16_t targetMinClear = 0, uint8_t targetMaxPercent = 75);

  // ======================================================================
  //  Color math (pure — ไม่แตะ I2C, ใช้ gain / ATIME ปัจจุบัน)
  // ======================================================================

  /**
   * @brief  แปลงเป็นสี 8-bit
   * @param  fullScale  0 = normalize ด้วยช่องที่มากที่สุด (ได้เฉดสีสด ไม่สนความสว่าง)
   *                    >0 = scale เทียบค่าเต็ม (ใช้ MASSMORE_TCS34725_CAL_SCALE กับข้อมูลที่ calibrate แล้ว)
   */
  RGB8 toRGB8(const RawData &d, bool gamma = false, uint16_t fullScale = 0) const;

  /**
   * @brief  แปลงเป็น HSV
   * @param  fullScale  ใช้คำนวณ V: 0 = C / getMaxCount(), >0 = max(R,G,B) / fullScale
   */
  HSV toHSV(const RawData &d, uint16_t fullScale = 0) const;

  /** @brief  Lux ตาม DN40 — NAN ถ้า saturated */
  float calculateLux(const RawData &d) const;

  /** @brief  CCT (Kelvin) — NAN ถ้า saturated หรือคำนวณไม่ได้ */
  float calculateColorTemperature(const RawData &d, CctMethod m = CctMethod::DN40) const;

  /** @brief  IR โดยประมาณ = (R + G + B − C) / 2 (ไม่ติดลบ) */
  float estimateIR(const RawData &d) const;

  /** @brief  ระดับที่ถือว่าอิ่มตัว (counts) — รวม analog, digital และ ripple 75% เมื่อ integration < 150 ms */
  uint16_t getSaturationLevel() const;

  /** @brief  true ถ้า clear channel ถึงระดับอิ่มตัว */
  bool isSaturated(const RawData &d) const;

  /** @brief  คำนวณ lux + CCT + IR + saturation ในครั้งเดียว */
  LightData calculateLight(const RawData &d) const;

  // ======================================================================
  //  Calibration
  // ======================================================================

  /** @brief  Glass Attenuation (GA ≥ 1.0) — ชดเชยกระจก / ฝาครอบที่บังแสง (DN40) */
  void setGlassAttenuation(float ga);
  /** @brief  Device Factor (DN40 ค่าเริ่มต้น 310) */
  void setDeviceFactor(float df);
  /** @brief  ค่าสัมประสิทธิ์ lux ของ DN40 (ค่าเริ่มต้น R 0.136, G 1.0, B −0.444) */
  void setLuxCoefficients(float rCoef, float gCoef, float bCoef);
  /** @brief  ค่าสัมประสิทธิ์ CCT ของ DN40 (ค่าเริ่มต้น CT_Coef 3810, CT_Offset 1391) */
  void setCctCoefficients(float ctCoef, float ctOffset);

  /**
   * @brief  บันทึกค่า dark offset (ปิดไฟ / ปิดหน้าเซ็นเซอร์แล้ววัด)
   * @note   จำ gain / ATIME ตอนบันทึกไว้ และ scale ให้อัตโนมัติถ้าเปลี่ยนภายหลัง
   */
  void setDarkOffset(const RawData &dark);
  /** @brief  บันทึก white reference (วางกระดาษขาว เปิด LED แล้ววัด) */
  void setWhiteReference(const RawData &white);
  /** @brief  ลบค่า calibration ทั้งหมด */
  void clearCalibration();
  /** @brief  มี white reference แล้วหรือยัง */
  bool hasWhiteReference() const { return _hasWhite; }

  /**
   * @brief  หัก dark offset และ normalize ด้วย white reference
   * @return ค่าที่ white reference = 10000 (MASSMORE_TCS34725_CAL_SCALE), จำกัดที่ 65535
   *         ถ้ายังไม่มี white reference จะหักแค่ dark offset
   */
  RawData applyCalibration(const RawData &d) const;

  // ======================================================================
  //  LED บนบอร์ด
  //  ค่าเริ่มต้น: ควบคุมผ่าน jumper INT → LED EN และ "ดับ" หลัง begin()
  //  ใช้งาน: tcs.ledOn();  tcs.ledOff();  tcs.ledToggle();
  // ======================================================================

  /** @brief  เปิด LED บนบอร์ด (เรียกก่อน begin() ได้ — จะมีผลตอน begin) */
  void ledOn()     { setLED(true); }
  /** @brief  ปิด LED บนบอร์ด (ค่าเริ่มต้น) */
  void ledOff()    { setLED(false); }
  /** @brief  สลับเปิด / ปิด LED */
  void ledToggle() { setLED(!_ledOn); }

  /**
   * @brief  เปิด / ปิด LED
   *         - โหมด INT (ค่าเริ่มต้น): เปิด = AIEN 0 (ทันที), ปิด = บังคับ AINT ทุก cycle + AIEN 1
   *           (LED ดับภายใน 1 RGBC cycle — ADC ต้องทำงานอยู่ ไลบรารีเปิดให้เอง)
   *         - attachLED(pin): digitalWrite ทันที (ปรับความสว่างใช้ PWM ใน sketch)
   */
  void setLED(bool on);

  /** @brief  สถานะ LED ล่าสุดที่สั่ง */
  bool getLED() const { return _ledOn; }

  /**
   * @brief  ใช้ขา INT ของชิปขับขา LED EN (ค่าเริ่มต้น ไม่ต้องเรียกเอง)
   * @note   INT เป็น open-drain active-low: ปล่อย INT = LED ติด (pull-up), ดึง INT ลง = LED ดับ
   *         ระหว่างใช้โหมดนี้ห้ามใช้ interrupt threshold / persistence เอง (ไลบรารีใช้แทน)
   */
  void attachLEDToINT();

  /** @brief  ต่อขา LED EN เข้า GPIO ของ MCU แทน jumper INT (ไลบรารีไม่กำหนดขาให้เอง) */
  void attachLED(int8_t pin, bool activeHigh = true);

  /**
   * @brief  เลิกให้ไลบรารีควบคุม LED และคืนขา INT ให้ใช้เป็น interrupt ตามปกติ (ดูตัวอย่าง 06)
   * @note   ถ้ายังต่อ jumper INT → LED EN อยู่ LED จะติด (pull-up) และดับเมื่อเกิด interrupt
   */
  void detachLED();

  // ======================================================================
  //  Diagnostics
  // ======================================================================

  /** @brief  error ล่าสุด */
  ErrorCode lastError() const { return _err; }
  /** @brief  ข้อความของ ErrorCode (อังกฤษ) */
  static const char *errorToString(ErrorCode e);

  /** @brief  อ่าน register (command type = repeated byte) */
  bool readRegister(uint8_t reg, uint8_t &val);
  /** @brief  เขียน register (ห้ามเขียน address ที่ datasheet ไม่ได้นิยาม) */
  bool writeRegister(uint8_t reg, uint8_t val);
  /** @brief  พิมพ์ทุก register ที่ datasheet นิยาม */
  void dumpRegisters(Print &out);

private:
  // ---- low-level bus ----
  bool _readBlock(uint8_t reg, uint8_t *buf, uint8_t len);
  bool _writeBlock(uint8_t reg, const uint8_t *buf, uint8_t len);
  bool _updateBits(uint8_t reg, uint8_t mask, uint8_t val);
  bool _sendCommand(uint8_t cmd);
  bool _readData(RawData &out);
  bool _restartCycle();
  bool _waitValid(uint32_t timeoutMs);
  bool _writeAllConfig();
  bool _ensureBegun();

  // ---- timing helpers ----
  uint32_t _cycleSteps() const;     ///< init + integration (+ wait) เป็นจำนวน step 2.4 ms
  uint32_t _timeoutMs() const;      ///< คำนวณจาก step max 2.56 ms + 10 ms margin
  uint32_t _minCycleMs() const;     ///< เวลาที่เร็วที่สุดที่ AVALID จะตั้งได้ (step min 2.27 ms)

  // ---- auto-range ----
  int8_t _findRangeIndex() const;
  bool   _rangeAdjust(const RawData &d);
  void   _applyRange(uint8_t idx);

  // ---- calibration helpers ----
  float  _sensitivity() const { return (float)getIntegrationCycles() * getGainValue(); }

  TwoWire  *_wire;
  uint8_t   _addr;
  ErrorCode _err;
  Variant   _variant;
  bool      _begun;
  bool      _powered;

  // config shadow (ค่าที่เขียนลงชิป ใช้กับคณิตสีแบบ const)
  uint8_t   _atime;
  Gain      _gain;
  uint8_t   _wtime;
  bool      _wlong;
  bool      _wen;
  uint8_t   _pers;
  uint16_t  _ailt, _aiht;
  bool      _aien;

  // FSM
  State     _state;
  bool      _continuous;
  bool      _newData;
  uint32_t  _t0;
  RawData   _data;

  // auto-range
  uint16_t  _arMinClear;
  uint8_t   _arMaxPct;

  // DN40 / calibration
  float     _ga, _df, _rCoef, _gCoef, _bCoef, _ctCoef, _ctOffset;
  RawData   _dark, _white;
  float     _darkSens, _whiteSens;
  bool      _hasDark, _hasWhite;

  // LED
  int8_t    _ledPin;
  bool      _ledActiveHigh;
  bool      _ledViaInt;
  bool      _ledOn;
};

#endif // MASSMORE_TCS34725_H
