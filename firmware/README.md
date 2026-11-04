# Massmore_TCS34725 — Factory Test Firmware (SKU-0067)

เฟิร์มแวร์ทดสอบบอร์ดก่อนส่งลูกค้า (Outgoing QA) สร้างจากตัวอย่าง `07_Factory_Test`
ผลลัพธ์บรรทัดที่ขึ้นต้นด้วย `#` อ่านอัตโนมัติได้ด้วย **Massmore Web Serial Monitor**

## Files

| File | Flash offset | Use |
|---|---|---|
| `bin/Massmore_TCS34725_FactoryTest_ESP32.bin` | `0x0` | Merged (bootloader + partitions + app) — แฟลชไฟล์เดียวจบ |
| `bin/Massmore_TCS34725_FactoryTest_ESP32_app.bin` | `0x10000` | App อย่างเดียว (บอร์ดที่มี bootloader อยู่แล้ว) |
| `manifest.json` | — | สำหรับ ESP Web Tools (แฟลชผ่านเบราว์เซอร์) |

Target: ESP32 classic (ESP32-WROOM-32 / DevKitC), 4 MB flash, Arduino-ESP32 Core 3.3.12

## Wiring

| Sensor (Qwiic) | ESP32 |
|---|---|
| 3V3 | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| INT ↔ LED EN | ใส่ jumper ไว้ (เฟิร์มแวร์ใช้ INT เปิด LED ส่องหน้าเซ็นเซอร์ระหว่างทดสอบ) |

I2C 100 kHz — วางบอร์ดในที่แสงนิ่ง ไม่ต้องมีวัตถุบังหน้าเซ็นเซอร์

## Flashing

**esptool (macOS / Linux / Windows)**

```bash
esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 512000 \
  write_flash 0x0 bin/Massmore_TCS34725_FactoryTest_ESP32.bin
```

ถ้าอัปโหลดไม่นิ่งให้ลด `--baud` เป็น `460800` หรือ `115200`

**PlatformIO** — เปิดโฟลเดอร์ `PlatformIO/` แล้วกด Upload (`src/main.cpp` คือ Factory Test)

**Web flasher** — ใช้ `manifest.json` กับ ESP Web Tools <!-- TODO: [MASSMORE_INPUT_REQUIRED: Massmore web flasher URL] -->

เปิด Serial Monitor ที่ **115200** — ทดสอบรันเองหลังบูต พิมพ์ `r` เพื่อทดสอบซ้ำ

## Expected Report (real board, 2026-11-05)

```text
#MASSMORE_FACTORY_TEST v1.0
#PRODUCT Massmore_TCS34725
#MCU ESP32
#RESULT BUS_SCAN PASS 0x29
#RESULT CHIP_ID PASS 0x44
#RESULT SERIAL PASS N/A
#RESULT REG_RW PASS 9/9
#RESULT INIT_TIMING PASS 27.5ms
#RESULT INTERRUPT_LOGIC PASS AINT_SET_CLEAR
#RESULT ADC_CEILING PASS 385
#RESULT GAIN_RATIO PASS 4.0/16.1/61.2
  (cycles=64 C@1x=402 C@60x=24600)
#RESULT AUTHENTICITY PASS GENUINE
#RESULT LED_BLINK PASS 5/5
  (on_min=1001 off_max=798 gain=16x)
#RESULT CONTINUOUS PASS 20/20
#RESULT RANGE_CLEAR PASS 24604
#RESULT RANGE_LUX PASS 200.0
#RESULT RANGE_CCT PASS 6837
#VERDICT PASS
[PASS] SENSOR QA PASSED - READY TO SHIP
info: gain=60x atime=153.6ms R=6426 G=9191 B=9150
Send 'r' to run again.
```

ระหว่าง `LED_BLINK` LED บนบอร์ดจะกระพริบ 5 ครั้ง (ให้ช่างมองยืนยันด้วยตา) — ค่า on/off ในวงเล็บขึ้นกับแสงรอบข้าง

ค่า RANGE_* / ADC_CEILING เปลี่ยนตามแสงรอบข้าง — สิ่งที่ต้องดูคือบรรทัด `#VERDICT`

## Fail Reasons

| REASON | ความหมาย / สิ่งที่ควรตรวจ |
|---|---|
| `NOT_FOUND` | ไม่มี ACK ที่ `0x29` — สาย Qwiic, ไฟเลี้ยง, pull-up |
| `CHIP_ID_MISMATCH` | ID ไม่ใช่ `0x44` (ถ้าได้ `0x00` / `0xFF` = บัสค้าง หรือไม่มี pull-up) |
| `WRONG_VARIANT` | ได้ `0x4D` = TCS34727 (I2C 1.8 V) ไม่ใช่ชิปของ SKU นี้ |
| `REG_RW_FAIL` | เขียน/อ่านกลับ register ไม่ตรง |
| `TIMING_OUT_OF_SPEC` | oscillator นอกสเปค 2.27–2.56 ms/step — สงสัยชิปเลียนแบบ |
| `INTERRUPT_FAIL` | AINT ไม่ตั้ง / ไม่หายหลัง clear |
| `ADC_CEILING_FAIL` | ค่าเกิน 1024 ที่ 1 cycle — ผิดสเปค ADC |
| `GAIN_OUT_OF_SPEC` | อัตราส่วน gain นอกช่วง datasheet (วัดได้แต่ค่าผิด) หรือแสงไม่นิ่ง |
| `LIGHT_TOO_HIGH` | แสงมากจน 60× อิ่มตัวแม้ปิด LED แล้ว (บรรทัด `led=off`) — เอามือ / วัตถุออกจากหน้าเซ็นเซอร์ บังแสงแรง แล้วพิมพ์ `r` (AUTHENTICITY จะขึ้น `INCONCLUSIVE` ไม่ใช่ชิปเสีย) |
| `LIGHT_TOO_LOW` | ค่าที่ 1× < 20 counts — ตรวจ LED / แสงรอบข้าง แล้วพิมพ์ `r` (AUTHENTICITY `INCONCLUSIVE`) |
| `AUTHENTICITY_SUSPECT` | ข้อใดข้อหนึ่งด้านบนไม่ผ่าน (ถ้าขึ้น `INCONCLUSIVE` = ไม่ผ่านเพราะแสงเท่านั้น ให้แก้แสงแล้วทดสอบซ้ำ) |
| `LED_FAIL` | LED บนบอร์ดไม่กระพริบ (ค่า clear ตอนเปิดสูงกว่าตอนปิดไม่ถึง 10 % + 20) — Serial จะแสดงกรอบ `WARNING` ให้ต่อ jumper INT → LED EN ก่อน แล้วตรวจ LED / แสงรอบข้าง จากนั้นพิมพ์ `r` |
| `CONTINUOUS_FAIL` | timeout / อิ่มตัว / noise > ±5 % (แสงกระพริบ?) |
| `RANGE_FAIL` | ค่า clear / lux / CCT นอกช่วงทางกายภาพ |
