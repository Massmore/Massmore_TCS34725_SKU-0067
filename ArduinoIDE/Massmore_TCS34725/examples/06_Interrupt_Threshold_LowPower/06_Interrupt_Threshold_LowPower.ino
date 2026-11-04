/**
 * 06_Interrupt_Threshold_LowPower — ตรวจจับแสงเปลี่ยนด้วย interrupt ของชิป + โหมดประหยัดพลังงาน
 *
 * แสดง: setInterruptThresholds(), setPersistence(), enableInterrupt(), clearInterrupt(),
 *        setWaitTime() (WEN / WLONG), isInterruptFlag(), readLatestRaw(), readRawOneShot(), powerOff()
 *
 * หลักการ: ชิปวัดเองเป็นรอบ (integration 24 ms + wait 500 ms ≈ กินไฟเฉลี่ยต่ำ)
 *          ถ้า clear channel ออกนอกช่วง ±THRESHOLD_PCT % ติดกัน 3 รอบ → AINT ตั้ง + ขา INT ดึงลง
 *          MCU ไม่ต้องอ่านค่าตลอดเวลา — ใช้ทำเซ็นเซอร์เปิด/ปิดไฟ, ฝากล่องเปิด, ของผ่านสายพาน
 *
 * การรับ interrupt (เลือก INT_PIN)
 *   INT_PIN = -1 : poll STATUS ผ่าน I2C ทุก 100 ms (ใช้แค่สาย Qwiic)
 *   INT_PIN >= 0 : ต่อขา INT ของบอร์ดเข้า GPIO → ISR (FALLING) — INT เป็น open-drain ต้องมี pull-up
 *   ค่าเริ่มต้นไลบรารีใช้ขา INT คุม LED → ตัวอย่างนี้เรียก detachLED() เพื่อใช้ INT เป็น interrupt
 *   บอร์ดที่ jumper INT → LED EN: LED จะติด และดับเมื่อเกิด interrupt (เป็นไฟแสดงสถานะในตัว)
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

#define INT_PIN        -1        // เช่น 27 (ESP32) หรือ 2 (Nano, INT0)
#define THRESHOLD_PCT  30        // ช่วงยอมรับ ±30 % รอบค่าปัจจุบัน
#define WAIT_MS        500.0f    // พักระหว่างรอบวัด (0 = วัดต่อเนื่อง, >614 ms ใช้ WLONG อัตโนมัติ)

#if defined(ESP32)
  #define ISR_ATTR IRAM_ATTR
#else
  #define ISR_ATTR
#endif

Massmore_TCS34725 tcs;
volatile bool intFired = false;

// ใส่ ISR_ATTR ที่ prototype ครั้งเดียว (ไม่ให้ IDE สร้าง prototype ซ้ำจน section ชนกัน)
void ISR_ATTR onTcsInterrupt();
void onTcsInterrupt() { intFired = true; }            // ISR สั้นที่สุด — งาน I2C ทำใน loop()

/// ตั้ง threshold รอบค่าปัจจุบัน (อ่านค่าใหม่หลัง clear interrupt เพื่อให้ได้ baseline ที่นิ่ง)
static void rearm() {
  tcs.clearInterrupt();                                  // ปล่อย INT (LED ติดกลับ ถ้าใช้ jumper)
  Massmore_TCS34725::RawData d;
  if (!tcs.readRaw(d)) return;
  const uint32_t span = (uint32_t)d.c * THRESHOLD_PCT / 100u + 5u;
  const uint16_t low  = (d.c > span) ? (uint16_t)(d.c - span) : 0;
  const uint32_t hi32 = (uint32_t)d.c + span;
  const uint16_t high = (hi32 > 65535u) ? 65535u : (uint16_t)hi32;
  tcs.setInterruptThresholds(low, high);
  tcs.clearInterrupt();
  Serial.print(F("armed: C=")); Serial.print(d.c);
  Serial.print(F("  window ")); Serial.print(low);
  Serial.print(F("..")); Serial.println(high);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println(F("\nMassmore_TCS34725 — 06_Interrupt_Threshold_LowPower"));

  Wire.begin();
  Wire.setClock(400000);
  if (!tcs.begin()) {
    Serial.println(F("Sensor not found"));
    while (true) { delay(1000); }
  }
  tcs.detachLED();                // คืนขา INT ให้ใช้เป็น interrupt (ไม่ให้ไลบรารีใช้คุม LED)

  // ---- ตัวอย่าง one-shot: ปลุก → วัด 1 รอบ → sleep (~2.5 µA) ----
  Massmore_TCS34725::RawData d;
  if (tcs.readRawOneShot(d)) {
    Serial.print(F("one-shot C=")); Serial.print(d.c);
    Serial.println(F(" (sensor sleeping)"));
  }

  // ---- ตั้งโหมด interrupt + low-power wait ----
  tcs.setIntegrationTime(24.0f);
  tcs.setGain(Massmore_TCS34725::Gain::X16);
  tcs.setPersistence(Massmore_TCS34725::Persistence::P3);   // ต้องผิดปกติ 3 รอบติดกัน กันสัญญาณรบกวน
  tcs.setWaitTime(WAIT_MS);
  Serial.print(F("cycle period ≈ "));
  Serial.print(2.4f + tcs.getIntegrationTimeMs() + tcs.getWaitTimeMs(), 1);
  Serial.println(F(" ms"));

#if INT_PIN >= 0
  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onTcsInterrupt, FALLING);
#endif
  rearm();
  tcs.enableInterrupt(true);
  tcs.enableRGBC(true);                                       // ชิปวัดเองเป็นรอบต่อจากนี้
}

void loop() {
  static uint32_t lastPoll = 0;
  bool fired = false;

#if INT_PIN >= 0
  if (intFired) { intFired = false; fired = true; }
#else
  if ((uint32_t)(millis() - lastPoll) >= 100u) {             // poll แบบ rollover-safe
    lastPoll = millis();
    fired = tcs.isInterruptFlag();
  }
#endif

  if (fired) {
    Massmore_TCS34725::RawData d;
    tcs.readLatestRaw(d);                                     // ค่าที่ทำให้เกิด interrupt (ไม่เริ่มรอบใหม่)
    Serial.print(F("LIGHT CHANGE  C=")); Serial.print(d.c);
    Serial.print(F("  lux=")); Serial.println(tcs.calculateLux(d), 1);
    rearm();                                                  // baseline ใหม่
    tcs.enableRGBC(true);
  }

  // MCU ว่าง: งานอื่นทำได้เต็มที่ หรือ ESP32 จะเข้า light-sleep ก็ได้
}
