/**
 * 03_NonBlocking_Multitask — อ่านเซ็นเซอร์แบบไม่ block loop()
 *
 * แสดง: startContinuous(), update(), isDataReady(), getReadings(), autoRangeStep()
 *   - loop() วิ่งหลายหมื่นรอบต่อวินาทีระหว่างที่ชิป integrate (ไม่มี delay ในไลบรารี)
 *   - งานอื่น (กระพริบ LED บนบอร์ด MCU) ทำงานตรงเวลาไปพร้อมกัน
 *   - autoRangeStep() ปรับ gain / integration ทีละขั้นตามแสง โดยไม่หยุดรอ
 *   - LED บนบอร์ดเซ็นเซอร์ (jumper INT → LED EN): ค่าเริ่มต้นดับ — ส่ง 'l' เพื่อเปิด/ปิด
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

#ifndef LED_BUILTIN
  #define LED_BUILTIN -1     // บอร์ดที่ไม่มี LED_BUILTIN: ข้ามการกระพริบ
#endif
#define BLINK_MS 250UL

Massmore_TCS34725 tcs;

uint32_t loopCount = 0;
uint32_t lastBlink = 0, lastReport = 0;
bool     ledState = false;
uint16_t samples = 0;

static void printGainName() {
  switch (tcs.getGain()) {
    case Massmore_TCS34725::Gain::X1:  Serial.print(F("1x"));  break;
    case Massmore_TCS34725::Gain::X4:  Serial.print(F("4x"));  break;
    case Massmore_TCS34725::Gain::X16: Serial.print(F("16x")); break;
    case Massmore_TCS34725::Gain::X60: Serial.print(F("60x")); break;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println(F("\nMassmore_TCS34725 — 03_NonBlocking_Multitask"));

  if (LED_BUILTIN >= 0) pinMode(LED_BUILTIN, OUTPUT);

  Wire.begin();
  Wire.setClock(400000);
  if (!tcs.begin()) {
    Serial.println(F("Sensor not found"));
    while (true) { delay(1000); }
  }
  tcs.setIntegrationTime(100.8f);           // 42 cycles
  tcs.setGain(Massmore_TCS34725::Gain::X4);
  tcs.ledOff();                             // LED บนบอร์ดเซ็นเซอร์: ledOn() = เปิด, ledOff() = ปิด
  Serial.println(F("Send 'l' = sensor LED on/off"));
  tcs.startContinuous();                    // ไม่ block — update() จะดูแลต่อ
}

void loop() {
  const uint32_t now = millis();
  loopCount++;

  // ---------- งานที่ 0: คำสั่งจาก Serial ----------
  if (Serial.available() && Serial.read() == 'l') {
    tcs.ledToggle();
    Serial.println(tcs.getLED() ? F(">> sensor LED ON") : F(">> sensor LED OFF"));
  }

  // ---------- งานที่ 1: เซ็นเซอร์ (ไม่ block) ----------
  tcs.update();
  if (tcs.isDataReady()) {
    const Massmore_TCS34725::RawData &d = tcs.getReadings();
    samples++;
    if (tcs.autoRangeStep()) {
      Serial.print(F("[auto-range] gain="));
      printGainName();
      Serial.print(F(" integration="));
      Serial.print(tcs.getIntegrationTimeMs(), 1);
      Serial.println(F(" ms"));
    } else {
      Serial.print(F("C=")); Serial.print(d.c);
      Serial.print(F(" R=")); Serial.print(d.r);
      Serial.print(F(" G=")); Serial.print(d.g);
      Serial.print(F(" B=")); Serial.print(d.b);
      Serial.print(F(" Lux=")); Serial.println(tcs.calculateLux(d), 1);
    }
  }

  // ---------- งานที่ 2: กระพริบ LED ตรงเวลา (rollover-safe) ----------
  if (LED_BUILTIN >= 0 && (uint32_t)(now - lastBlink) >= BLINK_MS) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  }

  // ---------- งานที่ 3: รายงานทุก 1 วินาที ----------
  if ((uint32_t)(now - lastReport) >= 1000UL) {
    lastReport = now;
    Serial.print(F("--- loop/s=")); Serial.print(loopCount);
    Serial.print(F("  samples/s=")); Serial.print(samples);
    Serial.print(F("  error=")); Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
    loopCount = 0;
    samples = 0;
  }
}
