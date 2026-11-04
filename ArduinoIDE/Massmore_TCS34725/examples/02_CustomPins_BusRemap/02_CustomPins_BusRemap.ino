/**
 * 02_CustomPins_BusRemap — กำหนดขา I2C เอง / ใช้บัสที่สอง / สแกนหา address
 *
 * แสดง:
 *   - ESP32 / ESP32-S3 (Core 3.x): Wire.begin(SDA, SCL, freq) เลือก GPIO ได้อิสระผ่าน GPIO matrix
 *   - ESP32 / ESP32-S3: ใช้ Wire1 เป็นบัสที่สอง (เช่นต่อเซ็นเซอร์ 2 ตัว เพราะ address 0x29 ตายตัว)
 *   - Arduino Nano: ขา I2C ตายตัวที่ A4 (SDA) / A5 (SCL)
 *   - สแกนบัสหา 0x29 (TCS34725/27) และ 0x39 (TCS34721/23) แล้ว begin() ด้วย address ที่เจอ
 *   - LED บนบอร์ดแต่ละตัว (jumper INT → LED EN): ค่าเริ่มต้นดับ — ส่ง 'l' เพื่อเปิด/ปิดทุกตัว
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

// ---------------- ตั้งค่าขา (แก้ตรงนี้ที่เดียว) ----------------
#if defined(ESP32)
  #if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define BUS0_SDA 8       // ESP32-S3: default ของ core
    #define BUS0_SCL 9
  #else
    #define BUS0_SDA 21      // ESP32 classic: Qwiic ของ Massmore
    #define BUS0_SCL 22
  #endif
  // บัสที่สอง (optional) — ตั้ง -1 ถ้าไม่ใช้
  #define BUS1_SDA -1        // เช่น 25
  #define BUS1_SCL -1        // เช่น 26
#endif
#define I2C_FREQ 400000UL    // TCS34725 รองรับสูงสุด 400 kHz (สายยาวให้ลดเป็น 100 kHz)

Massmore_TCS34725 sensor0;
#if defined(ESP32)
Massmore_TCS34725 sensor1;
bool hasSensor1 = false;
#endif

/// สแกนหา TCS3472x บนบัส คืน address ที่เจอ (0 = ไม่เจอ)
static uint8_t findTcs(TwoWire &bus, const __FlashStringHelper *name) {
  Serial.print(F("Scan "));
  Serial.print(name);
  Serial.print(F(": "));
  uint8_t found = 0, count = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) {
      Serial.print(F("0x"));
      if (a < 0x10) Serial.print('0');
      Serial.print(a, HEX);
      Serial.print(' ');
      count++;
      if (found == 0 && (a == MASSMORE_TCS34725_ADDR_DEFAULT || a == MASSMORE_TCS34725_ADDR_ALT)) found = a;
    }
  }
  if (count == 0) Serial.print(F("(no device)"));
  Serial.println();
  return found;
}

static void printReading(const __FlashStringHelper *name, Massmore_TCS34725 &s) {
  Massmore_TCS34725::RawData d;
  Serial.print(name);
  if (!s.readRaw(d)) {
    Serial.print(F(" error: "));
    Serial.println(Massmore_TCS34725::errorToString(s.lastError()));
    return;
  }
  Serial.print(F(" C=")); Serial.print(d.c);
  Serial.print(F(" R=")); Serial.print(d.r);
  Serial.print(F(" G=")); Serial.print(d.g);
  Serial.print(F(" B=")); Serial.println(d.b);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println(F("\nMassmore_TCS34725 — 02_CustomPins_BusRemap"));

#if defined(ESP32)
  // ESP32 Core 3.x: Wire.begin(sda, scl, frequency)
  Wire.begin(BUS0_SDA, BUS0_SCL, I2C_FREQ);
  Serial.print(F("Wire  SDA=")); Serial.print(BUS0_SDA);
  Serial.print(F(" SCL=")); Serial.println(BUS0_SCL);
#else
  // Arduino Nano (ATmega328P): ขาตายตัว A4 = SDA, A5 = SCL
  Wire.begin();
  Wire.setClock(I2C_FREQ);
  Serial.println(F("Wire  SDA=A4 SCL=A5 (fixed)"));
#endif

  const uint8_t addr0 = findTcs(Wire, F("Wire "));
  if (addr0 == 0 || !sensor0.begin(Wire, addr0)) {
    Serial.println(F("TCS3472x not found on Wire"));
  } else {
    Serial.print(F("sensor0 OK @0x")); Serial.println(addr0, HEX);
    sensor0.ledOff();             // LED บนบอร์ด: ledOn() = เปิด, ledOff() = ปิด (ค่าเริ่มต้น)
  }

#if defined(ESP32)
  if (BUS1_SDA >= 0 && BUS1_SCL >= 0) {
    Wire1.begin(BUS1_SDA, BUS1_SCL, I2C_FREQ);
    const uint8_t addr1 = findTcs(Wire1, F("Wire1"));
    hasSensor1 = addr1 != 0 && sensor1.begin(Wire1, addr1);
    Serial.println(hasSensor1 ? F("sensor1 OK on Wire1") : F("sensor1 not found on Wire1"));
    if (hasSensor1) sensor1.ledOff();
  }
#endif
  Serial.println(F("Send 'l' = LED on/off"));
}

void loop() {
  if (Serial.available() && Serial.read() == 'l') {
    sensor0.ledToggle();
#if defined(ESP32)
    if (hasSensor1) sensor1.ledToggle();
#endif
    Serial.println(sensor0.getLED() ? F(">> LED ON") : F(">> LED OFF"));
  }

  printReading(F("sensor0"), sensor0);
#if defined(ESP32)
  if (hasSensor1) printReading(F("sensor1"), sensor1);
#endif
  delay(1000);
}
