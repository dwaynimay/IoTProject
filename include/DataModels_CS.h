#pragma once
// =============================================================================
// DataModels_CS.h — Struct paket ESP-NOW untuk CS dengan M=32
//
// MASALAH: CSAccelPacket 3 axis × 32 float × 4 byte = 392 byte > 250 byte limit
// SOLUSI:  Split menjadi 4 paket kecil per window:
//
//   CSAccelPacket   ax, ay        → 6 + 2×32×4 + 2 = 264  byte ✗ masih >250!
//   → Split lagi: 1 axis per paket
//
//   CS1AxisPacket   1 axis        → 6 + 1×32×4 + 2 = 136  byte ✓
//   Dikirim 6x untuk: ax, ay, az, gx, gy, gz
//   + CSPpgPacket   ir            → 6 + 32×4 + 2 + 2 = 138 byte ✓
//
//   Total: 7 paket × ~137 byte = 959 byte per window (640ms)
//   Bandwidth: 959 / 0.64s = 1.5 KB/s (sangat ringan untuk ESP-NOW)
//
// Alternatif: kurangi N atau M untuk muat lebih sedikit paket.
// =============================================================================

#include <Arduino.h>
#include "DataModels.h"
#include "CS_Sensor.h"  // CS_N, CS_M

// ---------------------------------------------------------------------------
// Tipe paket CS — tambahkan ke PacketType di DataModels.h
// ---------------------------------------------------------------------------
static constexpr uint8_t PKT_CS_AX  = 0x10;
static constexpr uint8_t PKT_CS_AY  = 0x11;
static constexpr uint8_t PKT_CS_AZ  = 0x12;
static constexpr uint8_t PKT_CS_GX  = 0x13;
static constexpr uint8_t PKT_CS_GY  = 0x14;
static constexpr uint8_t PKT_CS_GZ  = 0x15;
static constexpr uint8_t PKT_CS_IR  = 0x16;

// ---------------------------------------------------------------------------
// CS1AxisPacket — 1 sinyal, M pengukuran
// Dipakai untuk: ax, ay, az, gx, gy, gz, ir
//
// Size: 6 (header) + CS_M×4 (measurements) + 2 (edge) = 136 byte ✓
// ---------------------------------------------------------------------------
struct __attribute__((packed)) CS1AxisPacket {
    PacketHeader header;       //  6 byte — header.type membedakan axis mana
    float        y[CS_M];      // 128 byte (32 float × 4)
    EdgeResult   edge;         //  2 byte
};                             // Total: 136 byte ✓

// ---------------------------------------------------------------------------
// CSPpgPacket — PPG dengan metadata HR
// Size: 6 + CS_M×4 + 1 + 1 + 2 = 138 byte ✓
// ---------------------------------------------------------------------------
struct __attribute__((packed)) CSPpgPacket {
    PacketHeader header;       //  6 byte
    float        y_ir[CS_M];   // 128 byte
    int8_t       heart_rate;   //  1 byte
    bool         ppg_valid;    //  1 byte
    EdgeResult   edge;         //  2 byte
};                             // Total: 138 byte ✓

// Konstanta yang dipakai CS_PHI_SEED
static constexpr uint32_t CS_PHI_SEED_DEPLOY = 42;