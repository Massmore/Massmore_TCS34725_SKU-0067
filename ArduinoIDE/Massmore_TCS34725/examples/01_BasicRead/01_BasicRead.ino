/**
 * 01_BasicRead — อ่านค่าสีแบบง่ายที่สุด (Simple Blocking API)
 *
 * แสดง: begin(), verifyChipID(), readRaw() (C,R,G,B ใน transaction เดียว),
 *        toRGB8() เป็น HEX, calculateLux() / calculateColorTemperature() (DN40), isSaturated()
 *        ledOn() / ledOff() / ledToggle() — ส่ง 'l' ใน Serial Monitor เพื่อเปิด/ปิด LED บนบอร์ด
 *
 * การต่อสาย (Qwiic / I2C)
 *   ESP32         : SDA = 21, SCL = 22, 3V3, GND   (Wire.begin() ใช้ขา default ของบอร์ด)
 *   ESP32-S3      : SDA = 8,  SCL = 9  (default ของ core)
 *   Arduino Nano  : SDA = A4, SCL = A5 (ตรวจสอบระดับแรงดันของบอร์ดก่อนต่อ 5 V)
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

Massmore_TCS34725 tcs;

static void printHex2(uint8_t v) {
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }

  Wire.begin();                 // sketch เป็นเจ้าของ I2C bus — ไลบรารีไม่เรียก Wire.begin() เอง
  Wire.setClock(400000);        // TCS34725 รองรับสูงสุด 400 kHz

  Serial.println(F("\nMassmore_TCS34725 — 01_BasicRead"));
  if (!tcs.begin()) {
    Serial.print(F("Sensor not found: "));
    Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
    Serial.println(F("ตรวจสายไฟ / SDA / SCL / pull-up แล้วกด reset"));
    while (true) { delay(1000); }
  }

  Serial.print(F("Chip ID = 0x"));
  printHex2(tcs.getChipID());
  Serial.println(tcs.getVariant() == Massmore_TCS34725::Variant::TCS34725 ? F(" (TCS34725)") : F(" (TCS34727)"));

  // integration 153.6 ms (64 cycles → max count 65535), gain 4× เหมาะกับแสงในห้อง
  tcs.setIntegrationTime(153.6f);
  tcs.setGain(Massmore_TCS34725::Gain::X4);

  // LED บนบอร์ด (jumper INT → LED EN) — ค่าเริ่มต้นดับ
  tcs.ledOff();                 // tcs.ledOn() = เปิด, tcs.ledOff() = ปิด, tcs.ledToggle() = สลับ
  Serial.println(F("Send 'l' = LED on/off"));
}

void loop() {
  if (Serial.available() && Serial.read() == 'l') {
    tcs.ledToggle();
    Serial.println(tcs.getLED() ? F(">> LED ON") : F(">> LED OFF"));
  }

  Massmore_TCS34725::RawData d;
  if (!tcs.readRaw(d)) {        // เริ่มรอบใหม่ → รอ AVALID (มี timeout) → อ่าน 8 ไบต์
    Serial.print(F("Read error: "));
    Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
    delay(500);
    return;
  }

  Massmore_TCS34725::RGB8 c = tcs.toRGB8(d);

  Serial.print(F("C=")); Serial.print(d.c);
  Serial.print(F(" R=")); Serial.print(d.r);
  Serial.print(F(" G=")); Serial.print(d.g);
  Serial.print(F(" B=")); Serial.print(d.b);
  Serial.print(F("  HEX=#"));
  printHex2(c.r); printHex2(c.g); printHex2(c.b);

  if (tcs.isSaturated(d)) {
    Serial.println(F("  SATURATED — ลด gain หรือ integration time"));
  } else {
    Serial.print(F("  Lux=")); Serial.print(tcs.calculateLux(d), 1);
    Serial.print(F("  CCT=")); Serial.print(tcs.calculateColorTemperature(d), 0);
    Serial.println(F(" K"));
  }
  delay(500);
}
