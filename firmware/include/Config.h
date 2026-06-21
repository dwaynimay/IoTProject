// File: firmware/include/Config.h

#pragma once
// =============================================================================
// Config.h — Titik Masuk Konfigurasi Tunggal
// =============================================================================
// File ini adalah satu-satunya yang perlu di-include oleh modul manapun
// untuk mendapatkan akses ke seluruh konfigurasi sistem dan utilitas dasar.
//
// PANDUAN SETUP CEPAT:
//   1. config/credentials.h → isi SSID, password WiFi, IP broker MQTT
//   2. config/hardware.h    → isi MAC address ketiga ESP32
//   3. config/features.h    → atur LOG_LEVEL dan fitur on/off
//   4. config/tuning.h      → atur timing & priority
//   5. Compile & upload
//
// STRUKTUR INCLUDE (urutan penting):
//   features.h   → #define LOG_LEVEL, flag fitur
//   Logger.h     → makro LOG_* (butuh LOG_LEVEL dari features.h)
//   credentials.h, hardware.h, tuning.h → konfigurasi hardware & jaringan
// =============================================================================

// Node Role — di-inject oleh platformio.ini, jangan diubah di sini
#define ROLE_SENSOR_IMU 1
#define ROLE_SENSOR_PPG 2
#define ROLE_GATEWAY 3

// ── Urutan include DI BAWAH INI TIDAK BOLEH DIUBAH ──────────────────────────
// features.h HARUS sebelum Logger.h karena Logger membaca LOG_LEVEL dari sana.

#include "config/features.h"    // (1) Flag fitur & LOG_LEVEL — HARUS PERTAMA
#include "utils/Logger.h"       // (2) Makro LOG_* — butuh LOG_LEVEL dari (1)

#include "config/credentials.h" // (3) WiFi & MQTT credentials
#include "config/hardware.h"    // (4) Pin, MAC address, alamat I2C
#include "config/tuning.h"      // (5) Timing, priority, stack size