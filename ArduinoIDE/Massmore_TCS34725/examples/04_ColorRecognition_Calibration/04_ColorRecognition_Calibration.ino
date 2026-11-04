/**
 * 04_ColorRecognition_Calibration — calibrate แล้วจำแนกสีวัตถุ + ควบคุม LED บนบอร์ด
 *
 * แสดง: setDarkOffset(), setWhiteReference(), applyCalibration(), toHSV(), toRGB8() + gamma,
 *        ledOn() / ledOff() (LED ผ่าน jumper INT — ค่าเริ่มต้น), PWM ความสว่าง (ledcAttach / analogWrite)
 *
 * ขั้นตอนใช้งาน (พิมพ์คำสั่งใน Serial Monitor 115200, ไม่ต้องกด Enter ก็ได้)
 *   1) ปิดหน้าเซ็นเซอร์ให้มืดสนิท → พิมพ์ d   (บันทึก dark offset — LED ดับระหว่างวัด)
 *   2) วางกระดาษขาวห่าง ~1 cm      → พิมพ์ w   (บันทึก white reference — LED ติด)
 *   3) วางวัตถุสีต่าง ๆ — จะแสดง HEX, HSV และชื่อสี
 *   l = เปิด/ปิด LED, + / - = ปรับความสว่าง (เฉพาะโหมด GPIO PWM), c = ล้าง calibration
 *
 * การต่อ LED ของบอร์ด (เลือก LED_MODE ด้านล่าง)
 *   LED_MODE_INT  : jumper ขา INT → LED EN (LED EN มี pull-up) — ค่าเริ่มต้นของไลบรารี ใช้ tcs.ledOn() / ledOff()
 *                   ไม่ต้องใช้ GPIO ของ MCU เพิ่ม (ใช้แค่สาย Qwiic)
 *   LED_MODE_GPIO : ต่อขา LED EN เข้ากับ GPIO ของ MCU → เปิด/ปิด + ปรับความสว่างด้วย PWM
 *   LED_MODE_NONE : ไม่ควบคุม LED
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

#define LED_MODE_NONE 0
#define LED_MODE_INT  1
#define LED_MODE_GPIO 2

#define LED_MODE      LED_MODE_INT
#define LED_GPIO_PIN  4            // ใช้เมื่อ LED_MODE_GPIO (Nano: ต้องเป็นขา PWM เช่น 3, 5, 6, 9, 10, 11)

Massmore_TCS34725 tcs;
bool    ledState = true;           // เริ่มด้วย LED ติด (ส่องวัตถุ)
uint8_t ledDuty = 255;

// ---------------------------------------------------------------------------
//  ตารางสี — แก้ / เพิ่มได้ตามงาน (hue เป็นองศา 0–360, ช่วง wrap ผ่าน 0 ได้)
// ---------------------------------------------------------------------------
struct ColorRule { const char *name; float hueFrom, hueTo; };
static const ColorRule kColors[] = {
  { "RED",     345.0f,  15.0f },
  { "ORANGE",   15.0f,  40.0f },
  { "YELLOW",   40.0f,  70.0f },
  { "GREEN",    70.0f, 165.0f },
  { "CYAN",    165.0f, 200.0f },
  { "BLUE",    200.0f, 255.0f },
  { "PURPLE",  255.0f, 290.0f },
  { "PINK",    290.0f, 345.0f },
};

static const char *classify(const Massmore_TCS34725::HSV &hsv) {
  // สีไม่อิ่ม (saturation ต่ำ) → แยกตามความสว่าง
  if (hsv.s < 0.18f) {
    if (hsv.v > 0.65f) return "WHITE";
    if (hsv.v < 0.12f) return "BLACK";
    return "GRAY";
  }
  if (hsv.v < 0.05f) return "BLACK";
  for (uint8_t i = 0; i < sizeof(kColors) / sizeof(kColors[0]); i++) {
    const ColorRule &r = kColors[i];
    const bool hit = (r.hueFrom <= r.hueTo) ? (hsv.h >= r.hueFrom && hsv.h < r.hueTo)
                                            : (hsv.h >= r.hueFrom || hsv.h < r.hueTo);
    if (hit) return r.name;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
//  LED
// ---------------------------------------------------------------------------
static void applyLed() {
#if LED_MODE == LED_MODE_INT
  if (ledState) tcs.ledOn(); else tcs.ledOff();
#elif LED_MODE == LED_MODE_GPIO
  const uint8_t duty = ledState ? ledDuty : 0;
  #if defined(ESP32)
    ledcWrite(LED_GPIO_PIN, duty);           // ESP32 Core 3.x: ledcWrite(pin, duty)
  #else
    analogWrite(LED_GPIO_PIN, duty);
  #endif
#endif
}

static void setupLed() {
#if LED_MODE == LED_MODE_GPIO
  tcs.detachLED();                           // ไม่ใช้ jumper INT → คืนขา INT
  #if defined(ESP32)
    ledcAttach(LED_GPIO_PIN, 5000, 8);        // ESP32 Core 3.x: ledcAttach(pin, freq, resolution)
  #else
    pinMode(LED_GPIO_PIN, OUTPUT);
  #endif
#elif LED_MODE == LED_MODE_NONE
  tcs.detachLED();
#endif
  applyLed();
}

/// วัดเฉลี่ย 4 ครั้ง (ลด noise ตอน calibrate)
static bool readAverage(Massmore_TCS34725::RawData &out) {
  uint32_t c = 0, r = 0, g = 0, b = 0;
  for (uint8_t i = 0; i < 4; i++) {
    Massmore_TCS34725::RawData d;
    if (!tcs.readRaw(d)) return false;
    c += d.c; r += d.r; g += d.g; b += d.b;
  }
  out.c = c / 4; out.r = r / 4; out.g = g / 4; out.b = b / 4;
  return true;
}

static void printHex2(uint8_t v) {
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

static void handleCommand(char cmd) {
  Massmore_TCS34725::RawData d;
  switch (cmd) {
    case 'd': {
      const bool prev = ledState;
      ledState = false; applyLed(); delay(50);   // dark: LED ต้องดับ
      if (readAverage(d)) {
        tcs.setDarkOffset(d);
        Serial.print(F(">> Dark offset saved  C=")); Serial.println(d.c);
      }
      ledState = prev; applyLed();
      break;
    }
    case 'w':
      ledState = true; applyLed(); delay(50);
      if (readAverage(d)) {
        if (tcs.isSaturated(d)) {
          Serial.println(F(">> White SATURATED — ลด gain / ขยับกระดาษออกห่าง"));
        } else {
          tcs.setWhiteReference(d);
          Serial.print(F(">> White reference saved  C=")); Serial.println(d.c);
        }
      }
      break;
    case 'c': tcs.clearCalibration(); Serial.println(F(">> Calibration cleared")); break;
    case 'l': ledState = !ledState; applyLed(); Serial.println(ledState ? F(">> LED ON") : F(">> LED OFF")); break;
    case '+': ledDuty = (ledDuty > 230) ? 255 : ledDuty + 25; applyLed(); Serial.print(F(">> duty=")); Serial.println(ledDuty); break;
    case '-': ledDuty = (ledDuty < 25) ? 0 : ledDuty - 25;    applyLed(); Serial.print(F(">> duty=")); Serial.println(ledDuty); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println(F("\nMassmore_TCS34725 — 04_ColorRecognition_Calibration"));
  Serial.println(F("d=dark  w=white  c=clear cal  l=LED on/off  +/-=PWM"));

  Wire.begin();
  Wire.setClock(400000);
  if (!tcs.begin()) {
    Serial.println(F("Sensor not found"));
    while (true) { delay(1000); }
  }
  tcs.setIntegrationTime(48.0f);             // 20 cycles — ตอบสนองเร็วพอสำหรับคัดแยกสี
  tcs.setGain(Massmore_TCS34725::Gain::X16);
  setupLed();
}

void loop() {
  while (Serial.available()) handleCommand((char)Serial.read());

  Massmore_TCS34725::RawData raw;
  if (!tcs.readRaw(raw)) {
    Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
    delay(300);
    return;
  }

  const bool calibrated = tcs.hasWhiteReference();
  const Massmore_TCS34725::RawData cal = tcs.applyCalibration(raw);
  const uint16_t scale = calibrated ? MASSMORE_TCS34725_CAL_SCALE : 0;
  const Massmore_TCS34725::HSV hsv = tcs.toHSV(cal, scale);
  const Massmore_TCS34725::RGB8 rgb = tcs.toRGB8(cal, false, scale);
  const Massmore_TCS34725::RGB8 led = tcs.toRGB8(cal, true, scale);   // gamma สำหรับส่งให้ RGB LED

  Serial.print(calibrated ? F("[CAL] ") : F("[RAW] "));
  Serial.print(F("#")); printHex2(rgb.r); printHex2(rgb.g); printHex2(rgb.b);
  Serial.print(F("  H=")); Serial.print(hsv.h, 0);
  Serial.print(F(" S=")); Serial.print(hsv.s, 2);
  Serial.print(F(" V=")); Serial.print(hsv.v, 2);
  Serial.print(F("  LED-gamma=#")); printHex2(led.r); printHex2(led.g); printHex2(led.b);
  Serial.print(F("  -> "));
  Serial.print(classify(hsv));
  if (tcs.isSaturated(raw)) Serial.print(F("  (SATURATED)"));
  Serial.println();
  delay(250);
}
