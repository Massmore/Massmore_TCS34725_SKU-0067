/**
 * 05_LightMeter_Lux_CCT_AutoRange — เครื่องวัดแสง lux / อุณหภูมิสี พร้อม auto-range
 *
 * แสดง: autoRange(), calculateLight() (DN40 lux + CCT + IR + saturation), isSaturated(),
 *        estimateIR(), setGlassAttenuation(), calculateColorTemperature(McCamy)
 *
 * Output เป็นรูปแบบ Serial Plotter (label:value) — เปิด Tools → Serial Plotter ดูกราฟได้
 * LED บนบอร์ด (jumper INT → LED EN) ต้องดับ เพื่อไม่ให้แสง LED ปนกับแสงที่วัด (ไลบรารีดับให้เป็นค่าเริ่มต้น)
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 */
#include <Arduino.h>
#include <Wire.h>
#include <Massmore_TCS34725.h>

#define GLASS_ATTENUATION  1.0f   // มีฝาครอบ / กระจกบังเซ็นเซอร์ → ใส่ค่า > 1 (เช่น 1.4)

Massmore_TCS34725 tcs;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println(F("\nMassmore_TCS34725 — 05_LightMeter_Lux_CCT_AutoRange"));

  Wire.begin();
  Wire.setClock(400000);
  if (!tcs.begin()) {
    Serial.println(F("Sensor not found"));
    while (true) { delay(1000); }
  }
  tcs.ledOff();                       // วัดแสงรอบข้าง → LED ต้องดับ (ค่าเริ่มต้นอยู่แล้ว เขียนไว้ให้ชัด)
  tcs.setGlassAttenuation(GLASS_ATTENUATION);

  // หา gain + integration ที่เหมาะสมครั้งแรก (blocking สูงสุด ~8 รอบวัด)
  if (!tcs.autoRange()) {
    Serial.print(F("autoRange: "));
    Serial.println(Massmore_TCS34725::errorToString(tcs.lastError()));
  }
}

void loop() {
  Massmore_TCS34725::RawData d;
  if (!tcs.readRaw(d)) {
    Serial.print(F("error:"));
    Serial.println((int)tcs.lastError());
    delay(500);
    return;
  }

  // แสงเปลี่ยนมาก (อิ่มตัว หรือมืดเกิน 1% ของ max count) → auto-range ใหม่
  // (ถ้าไวสุดแล้ว 60× / 614 ms ก็ไม่ต้องทำซ้ำ — มืดจริง)
  const bool atMaxSensitivity = tcs.getGain() == Massmore_TCS34725::Gain::X60 &&
                                tcs.getIntegrationCycles() == 256;
  const bool tooDark = d.c < tcs.getMaxCount() / 100u && !atMaxSensitivity;
  if (tcs.isSaturated(d) || tooDark) {
    tcs.autoRange();
    return;
  }

  const Massmore_TCS34725::LightData L = tcs.calculateLight(d);
  Serial.print(F("lux:"));       Serial.print(L.lux, 1);
  Serial.print(F(",cct_dn40:")); Serial.print(L.cct, 0);
  Serial.print(F(",cct_mccamy:"));
  Serial.print(tcs.calculateColorTemperature(d, Massmore_TCS34725::CctMethod::MCCAMY), 0);
  Serial.print(F(",ir:"));       Serial.print(L.ir, 0);
  Serial.print(F(",clear:"));    Serial.print(d.c);
  Serial.print(F(",gain:"));     Serial.print(tcs.getGainValue(), 0);
  Serial.print(F(",atime_ms:")); Serial.print(tcs.getIntegrationTimeMs(), 1);
  Serial.print(F(",valid:"));    Serial.println(L.valid ? 1 : 0);
  delay(200);
}
