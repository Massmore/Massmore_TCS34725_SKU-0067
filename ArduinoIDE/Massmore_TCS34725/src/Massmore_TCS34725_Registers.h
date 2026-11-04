/**
 * @file    Massmore_TCS34725_Registers.h
 * @brief   Register map, bitmask และ command constants ของ TCS34725 / TCS34727
 *
 * อ้างอิง: ams-OSRAM TCS3472 Color Light-to-Digital Converter Datasheet (DS000390)
 * ทุกค่าในไฟล์นี้มาจาก datasheet โดยตรง — ห้ามแก้ถ้าไม่ได้ตรวจกับ datasheet
 *
 * Designed and Manufactured by Massmore — https://www.massmore.shop
 * SPDX-License-Identifier: MIT
 */
#ifndef MASSMORE_TCS34725_REGISTERS_H
#define MASSMORE_TCS34725_REGISTERS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
//  Command byte (datasheet: "Command Register")
//  bit7 CMD = 1 ทุกครั้ง, bit6:5 TYPE, bit4:0 ADDR/SF
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_CMD_BIT            0x80  ///< ต้องตั้งทุกครั้งที่เขียน command byte
#define MASSMORE_TCS34725_CMD_REPEATED       0x00  ///< TYPE 00 — Repeated byte (อ่าน/เขียน register เดิมซ้ำ)
#define MASSMORE_TCS34725_CMD_AUTO_INC       0x20  ///< TYPE 01 — Auto-increment (block read/write)
#define MASSMORE_TCS34725_CMD_SPECIAL        0x60  ///< TYPE 11 — Special function
#define MASSMORE_TCS34725_SF_CLEAR_INT       0x06  ///< Special function 00110 — RGBC clear-channel interrupt clear
/// Command byte สำเร็จรูปสำหรับ clear interrupt = 0x80 | 0x60 | 0x06 = 0xE6
#define MASSMORE_TCS34725_CMD_CLEAR_INT      (MASSMORE_TCS34725_CMD_BIT | MASSMORE_TCS34725_CMD_SPECIAL | MASSMORE_TCS34725_SF_CLEAR_INT)

// ---------------------------------------------------------------------------
//  Register addresses
//  Register ที่ไม่ได้นิยามใน datasheet (0x02, 0x08–0x0B, 0x0E, 0x10, 0x11) ห้ามเขียน
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_REG_ENABLE         0x00
#define MASSMORE_TCS34725_REG_ATIME          0x01
#define MASSMORE_TCS34725_REG_WTIME          0x03
#define MASSMORE_TCS34725_REG_AILTL          0x04
#define MASSMORE_TCS34725_REG_AILTH          0x05
#define MASSMORE_TCS34725_REG_AIHTL          0x06
#define MASSMORE_TCS34725_REG_AIHTH          0x07
#define MASSMORE_TCS34725_REG_PERS           0x0C
#define MASSMORE_TCS34725_REG_CONFIG         0x0D
#define MASSMORE_TCS34725_REG_CONTROL        0x0F
#define MASSMORE_TCS34725_REG_ID             0x12
#define MASSMORE_TCS34725_REG_STATUS         0x13
#define MASSMORE_TCS34725_REG_CDATAL         0x14  ///< C, R, G, B ต่อกัน 8 ไบต์ (little-endian) 0x14–0x1B
#define MASSMORE_TCS34725_REG_CDATAH         0x15
#define MASSMORE_TCS34725_REG_RDATAL         0x16
#define MASSMORE_TCS34725_REG_RDATAH         0x17
#define MASSMORE_TCS34725_REG_GDATAL         0x18
#define MASSMORE_TCS34725_REG_GDATAH         0x19
#define MASSMORE_TCS34725_REG_BDATAL         0x1A
#define MASSMORE_TCS34725_REG_BDATAH         0x1B

// ---------------------------------------------------------------------------
//  ENABLE (0x00) — bit 7:5 และ bit 2 เป็น reserved ต้องเขียนเป็น 0
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_ENABLE_PON         0x01  ///< Power ON (internal oscillator)
#define MASSMORE_TCS34725_ENABLE_AEN         0x02  ///< RGBC ADC enable
#define MASSMORE_TCS34725_ENABLE_WEN         0x08  ///< Wait timer enable
#define MASSMORE_TCS34725_ENABLE_AIEN        0x10  ///< RGBC interrupt enable (ขา INT)
#define MASSMORE_TCS34725_ENABLE_MASK        0x1B  ///< bit ที่ใช้งานได้ทั้งหมด

// ---------------------------------------------------------------------------
//  PERS (0x0C) / CONFIG (0x0D) / CONTROL (0x0F) / STATUS (0x13)
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_PERS_MASK          0x0F  ///< APERS bit 3:0
#define MASSMORE_TCS34725_CONFIG_WLONG       0x02  ///< Wait time ×12
#define MASSMORE_TCS34725_CONFIG_MASK        0x02
#define MASSMORE_TCS34725_CONTROL_AGAIN_MASK 0x03  ///< AGAIN bit 1:0 (1/4/16/60×)
#define MASSMORE_TCS34725_STATUS_AVALID      0x01  ///< RGBC integration cycle เสร็จแล้ว
#define MASSMORE_TCS34725_STATUS_AINT        0x10  ///< RGBC clear-channel interrupt

// ---------------------------------------------------------------------------
//  Device ID (0x12)
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_ID_TCS34725        0x44  ///< TCS34721 / TCS34725 (VBUS = VDD)
#define MASSMORE_TCS34725_ID_TCS34727        0x4D  ///< TCS34723 / TCS34727 (VBUS = 1.8 V)

// ---------------------------------------------------------------------------
//  Timing (datasheet: Electrical Characteristics)
// ---------------------------------------------------------------------------
#define MASSMORE_TCS34725_STEP_US_TYP        2400u ///< ADC integration step (typ)
#define MASSMORE_TCS34725_STEP_US_MIN        2270u ///< ADC integration step (min)
#define MASSMORE_TCS34725_STEP_US_MAX        2560u ///< ADC integration step (max)
#define MASSMORE_TCS34725_WARMUP_US          2400u ///< เวลาหลังตั้ง PON ก่อนตั้ง AEN
#define MASSMORE_TCS34725_WLONG_FACTOR       12u   ///< WLONG = 1 คูณ wait time ×12

#endif // MASSMORE_TCS34725_REGISTERS_H
