# Massmore_TCS34725 — RGB Color Sensor (SKU-0067)

<!-- TODO: [MASSMORE_INPUT_REQUIRED: product image docs/images/Massmore_TCS34725_SKU-0067.jpg] -->
![Massmore TCS34725 SKU-0067](docs/images/Massmore_TCS34725_SKU-0067.jpg)

ไลบรารี Arduino / PlatformIO สำหรับบอร์ดเซ็นเซอร์สี **ams-OSRAM TCS34725** ของ Massmore
ใช้แค่ `Wire` ไม่ต้องติดตั้งไลบรารีอื่น ไม่มี dynamic allocation ใช้ได้ทั้ง ESP32, ESP32-S3 และ Arduino Nano

*Designed and Manufactured by Massmore*

---

## Product Overview

TCS34725 วัดแสง 4 ช่อง (Clear, Red, Green, Blue) ความละเอียด 16-bit พร้อมฟิลเตอร์ตัด IR ในตัวชิป
เหมาะกับงานคัดแยกสี, line / color tracking, วัดความสว่าง (lux) และอุณหภูมิสีของหลอดไฟ (CCT)

| Spec | Value |
|---|---|
| Sensor | ams-OSRAM TCS34725 (ID `0x44`) |
| Interface | I2C, address `0x29` (fixed), up to 400 kHz |
| Channels | Clear, Red, Green, Blue — 16-bit each |
| Gain | 1× / 4× / 16× / 60× |
| Integration time | 2.4 – 614.4 ms (step 2.4 ms) |
| Interrupt | Clear-channel threshold + persistence, open-drain active-low |
| Sleep current | ~2.5 µA ([datasheet](https://look.ams-osram.com/asset/c445632e-47db-4586-8b15-6b637d2dc915/TCS3472-DS000390.pdf)) |
| Connector | Qwiic-compatible 4-pin <!-- TODO: [MASSMORE_INPUT_REQUIRED: SKU-0067 connector count / VIN range / LDO spec] --> |

📄 **Datasheet:** [ams-OSRAM TCS3472 (DS000390)](https://look.ams-osram.com/asset/c445632e-47db-4586-8b15-6b637d2dc915/TCS3472-DS000390.pdf)

### จุดเด่นของไลบรารี

- อ่าน C, R, G, B แบบ **block 8 ไบต์ใน transaction เดียว** → ทุกช่องมาจาก integration cycle เดียวกัน
- **ไม่มี `delay()` ยาวในไลบรารี** — มี Blocking API (poll + timeout จริง) และ Non-blocking FSM
- **Lux / CCT ตาม ams DN40** (IR rejection, Glass Attenuation, Device Factor) + ตรวจ saturation (analog / digital / ripple)
- **Auto-range** gain + integration time ทั้งแบบ blocking และทีละขั้นแบบ non-blocking
- **Calibration** dark offset + white reference (ยังใช้ได้แม้ gain / integration เปลี่ยน)
- **ควบคุม LED บนบอร์ดผ่านขา INT** (jumper INT → LED EN) โดยไม่ต้องใช้ GPIO เพิ่ม — ดับเป็นค่าเริ่มต้น, `ledOn()` / `ledOff()`
- **ตรวจของแท้แบบ heuristic**: ID + register R/W + timing ของ oscillator + interrupt logic

---

## Pinout

<!-- TODO: [MASSMORE_INPUT_REQUIRED: SKU-0067 pinout — confirm pin names, VIN range, on-board I2C pull-up] -->

| Pin | Function | Notes |
|---|---|---|
| VIN / 3V3 | Power | <!-- TODO: [MASSMORE_INPUT_REQUIRED: VIN range] --> |
| GND | Ground | |
| SDA | I2C data | Qwiic |
| SCL | I2C clock | Qwiic |
| INT | Interrupt output | Open-drain, active-low — ต้องมี pull-up |
| LED (EN) | เปิด/ปิด LED ส่องวัตถุ | มี pull-up บนบอร์ด → ไม่ต่อ jumper = LED ติดตลอด |
| INT ↔ LED jumper | ต่อ INT เข้า LED EN | ให้ชิปสั่ง LED ผ่าน I2C — ไลบรารีดับ LED หลัง `begin()` |

---

## MCU Compatibility & Limitation Matrix

| MCU Platform | Tested Core / Toolchain | Bus Remapping Support | Limitations / Notes |
|---|---|---|---|
| **ESP32-S3** | Arduino-ESP32 v3.3.x (pioarduino 55.03.312) | Full GPIO Matrix, `Wire` / `Wire1` | Default SDA 8 / SCL 9. Compile-verified (hardware test pending). |
| **ESP32 (Classic)** | Arduino-ESP32 v3.3.x (pioarduino 55.03.312) | Full GPIO Matrix, `Wire` / `Wire1` | None. Primary Factory Test target (SDA 21 / SCL 22). |
| **AVR — Arduino Nano (ATmega328P)** | Arduino AVR Core | Fixed Hardware Pins (I2C: A4/A5) | 2 KB SRAM / 32 KB Flash. ชิป TCS34725 ทน VDD สูงสุด 3.6 V — ใช้กับ 5 V ได้เฉพาะบอร์ดที่มี 3.3 V LDO + I2C level shifting <!-- TODO: [MASSMORE_INPUT_REQUIRED: board regulator / level shifter spec] --> |

---

## Installation

**Arduino IDE** — ดาวน์โหลด [`ArduinoIDE/Massmore_TCS34725.zip`](ArduinoIDE/Massmore_TCS34725.zip)
แล้วเลือก *Sketch → Include Library → Add .ZIP Library…* (ESP32 ใช้ board package **3.3.x** ขึ้นไป)

**PlatformIO (VS Code)** — เปิดโฟลเดอร์ [`PlatformIO/`](PlatformIO/) แล้วกด Build / Upload ได้ทันที
ไลบรารีอยู่ใน `PlatformIO/lib/` แล้ว ไม่ต้องติดตั้งอะไรเพิ่ม (`src/main.cpp` = Factory Test)

---

## Quick Start

```cpp
#include <Wire.h>
#include <Massmore_TCS34725.h>

Massmore_TCS34725 tcs;

void setup() {
  Serial.begin(115200);
  Wire.begin();                       // sketch เป็นเจ้าของ I2C bus
  if (!tcs.begin()) {
    Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
    while (true) {}
  }
}

void loop() {
  Massmore_TCS34725::RawData d;
  if (tcs.readRaw(d)) {               // รอบใหม่ → รอ AVALID (มี timeout) → อ่าน 8 ไบต์
    Serial.print("C="); Serial.print(d.c);
    Serial.print(" R="); Serial.print(d.r);
    Serial.print(" G="); Serial.print(d.g);
    Serial.print(" B="); Serial.print(d.b);
    Serial.print(" Lux="); Serial.print(tcs.calculateLux(d), 1);
    Serial.print(" CCT="); Serial.println(tcs.calculateColorTemperature(d), 0);
  }
  delay(500);
}
```

---

## Pin Mapping Examples

```cpp
// ESP32 (Core 3.x) — เลือก GPIO ได้อิสระ
Wire.begin(21, 22, 400000);           // SDA, SCL, Hz
tcs.begin(Wire);
```

```cpp
// ESP32-S3 — บัสที่สองบน Wire1
Wire1.begin(8, 9, 400000);
tcs.begin(Wire1);
```

```cpp
// Arduino Nano — ขาตายตัว A4 (SDA) / A5 (SCL)
Wire.begin();
Wire.setClock(400000);
tcs.begin();
```

### ควบคุม LED บนบอร์ด (jumper INT → LED EN)

ต่อ jumper INT → LED EN ไว้ แล้วสั่งได้เลย — **ค่าเริ่มต้นหลัง `begin()` LED ดับ**

```cpp
tcs.begin();
tcs.ledOn();       // เปิด LED ส่องวัตถุ (ติดทันที)
tcs.ledOff();      // ปิด LED (ดับภายใน 1 RGBC cycle)
tcs.ledToggle();   // สลับ
tcs.getLED();      // สถานะล่าสุด true / false
```

> ไลบรารีใช้ interrupt threshold / persistence ของชิปขับขา INT จึงใช้ interrupt แจ้งเตือนแสงพร้อมกันไม่ได้
> ถ้าจะใช้ INT เป็น interrupt เอง ให้เรียก `tcs.detachLED()` ก่อน (ดูตัวอย่าง 06)
> ถ้าต่อ LED EN เข้า GPIO ของ MCU ให้ใช้ `attachLED(pin)` แทน (ปรับความสว่างด้วย PWM ได้ — ดูตัวอย่าง 04)
> ถ้าไม่ได้ต่อ jumper LED จะติดตลอด (LED EN มี pull-up) และคำสั่ง `ledOn()` / `ledOff()` จะไม่มีผล

---

## API Reference

| Function | Description | Return |
|---|---|---|
| `begin(wire, addr)` | ตรวจ ACK + ID, เขียน config ทั้งหมด, Power ON, เริ่มวัด | `bool` |
| `readRaw(d)` | เริ่มรอบใหม่ รอ AVALID แล้วอ่าน C,R,G,B (blocking + timeout) | `bool` |
| `readLatestRaw(d)` | อ่านค่าล่าสุดทันที ไม่เริ่มรอบใหม่ (โหมด interrupt / wait) | `bool` |
| `readRawOneShot(d)` | ปลุก → วัด 1 รอบ → sleep | `bool` |
| `readLux()` / `readColorTemperature()` | อ่าน + คำนวณ DN40 | `float` / `NAN` |
| `readRGB8(c, gamma)` | อ่านสี 8-bit (ใช้ white reference ถ้ามี) | `bool` |
| `startContinuous()` / `requestConversion()` | เริ่ม FSM ต่อเนื่อง / one-shot แบบไม่ block | `bool` |
| `update()` / `isDataReady()` / `getReadings()` | ขับ FSM / มีข้อมูลใหม่ / ผลล่าสุด | — / `bool` / `RawData` |
| `autoRange()` / `autoRangeStep()` | ปรับ gain + integration อัตโนมัติ (blocking / ทีละขั้น) | `bool` |
| `setIntegrationTime(ms)` / `setIntegrationCycles(n)` / `setATIME(raw)` | ตั้ง integration time | `bool` / — |
| `setGain(g)` | 1× / 4× / 16× / 60× | — |
| `setWaitTime(ms)` | พักระหว่างรอบ (WLONG อัตโนมัติเมื่อ > 614 ms) | `bool` |
| `setInterruptThresholds(lo, hi)` / `setPersistence(p)` / `enableInterrupt(on)` / `clearInterrupt()` | Interrupt | — |
| `toRGB8(d, gamma, fullScale)` / `toHSV(d, fullScale)` | แปลงสี | `RGB8` / `HSV` |
| `calculateLux(d)` / `calculateColorTemperature(d, m)` / `calculateLight(d)` | DN40 lux / CCT (DN40 หรือ McCamy) | `float` / `LightData` |
| `isSaturated(d)` / `getSaturationLevel()` / `estimateIR(d)` | ตรวจอิ่มตัว / ระดับอิ่มตัว / IR | `bool` / `uint16_t` / `float` |
| `setDarkOffset(d)` / `setWhiteReference(d)` / `applyCalibration(d)` | Calibration (white = 10000) | — / `RawData` |
| `setGlassAttenuation(ga)` / `setDeviceFactor(df)` | ค่าคงที่ DN40 | — |
| `ledOn()` / `ledOff()` / `ledToggle()` / `getLED()` | เปิด / ปิด / สลับ LED บนบอร์ด (ค่าเริ่มต้น: ดับ ผ่าน INT) | — / `bool` |
| `attachLEDToINT()` / `attachLED(pin)` / `detachLED()` / `setLED(on)` | เลือกวิธีต่อ LED (INT = ค่าเริ่มต้น / GPIO / ไม่ใช้) | — |
| `verifyChipID()` / `getVariant()` / `isGenuine()` | ตรวจ ID / รุ่น / ของแท้ (heuristic) | `bool` / `Variant` / `bool` |
| `getSerialNumber()` | ชิปไม่มี serial → คืน 0 เสมอ | `uint32_t` |
| `lastError()` / `errorToString(e)` / `dumpRegisters(out)` | Diagnostics | `ErrorCode` / `const char*` / — |

---

## Examples

| # | Example | Description |
|---|---|---|
| 01 | `01_BasicRead` | อ่าน C/R/G/B, HEX, lux, CCT แบบง่ายที่สุด |
| 02 | `02_CustomPins_BusRemap` | กำหนดขา I2C เอง, `Wire1`, Nano A4/A5, สแกนหา `0x29` / `0x39` |
| 03 | `03_NonBlocking_Multitask` | FSM ไม่ block loop + กระพริบ LED + auto-range ทีละขั้น |
| 04 | `04_ColorRecognition_Calibration` | Dark / white calibration, HSV, จำแนกชื่อสี, เปิด/ปิด LED, PWM |
| 05 | `05_LightMeter_Lux_CCT_AutoRange` | เครื่องวัด lux / CCT + auto-range, รูปแบบ Serial Plotter |
| 06 | `06_Interrupt_Threshold_LowPower` | Interrupt เมื่อแสงเปลี่ยน + persistence + wait (low power) + one-shot |
| 07 | `07_Factory_Test` | QA ก่อนส่งของ — ผลแบบ machine-readable |

---

## Factory Test & Web Serial Monitor

ดูวิธีแฟลชเฟิร์มแวร์ทดสอบและตัวอย่างผลจากบอร์ดจริงที่ [`firmware/README.md`](firmware/README.md)

---

## Where to Buy

- massmore.shop: <!-- TODO: [MASSMORE_INPUT_REQUIRED: massmore.shop product URL] -->
- Shopee: <!-- TODO: [MASSMORE_INPUT_REQUIRED: Shopee link] -->
- Lazada: <!-- TODO: [MASSMORE_INPUT_REQUIRED: Lazada link] -->

---

## References

- [ams-OSRAM TCS3472 Datasheet (DS000390)](https://look.ams-osram.com/asset/c445632e-47db-4586-8b15-6b637d2dc915/TCS3472-DS000390.pdf) — register map, gain / timing spec ที่ไลบรารีและ Factory Test ใช้อ้างอิง

---

## License

MIT License — © 2026 Massmore Biz Co., Ltd. ดูไฟล์ [`LICENSE`](ArduinoIDE/Massmore_TCS34725/LICENSE)
