#pragma once
// =============================================================================
// Config.h — Titik masuk konfigurasi
// =============================================================================
// File ini TIDAK perlu diubah untuk setup biasa.
// Semua pengaturan ada di subfolder config/:
//
//   config/credentials.h  → WiFi SSID/password, MQTT broker IP & port
//   config/hardware.h     → pin I2C, MAC address ESP32, alamat sensor
//   config/features.h     → aktifkan/nonaktifkan fitur (finger gate, batching)
//   config/tuning.h       → timing, task priority, stack size (pengguna mahir)
//
// PANDUAN SETUP CEPAT:
//   1. Buka config/credentials.h → isi SSID, password WiFi, dan IP broker
//   2. Buka config/hardware.h    → isi MAC address ketiga ESP32
//   3. Compile & upload
//
// Tidak perlu menyentuh file lain untuk memulai.
// =============================================================================

// Node Role — di-inject oleh platformio.ini, jangan diubah di sini
#define ROLE_SENSOR  1
#define ROLE_GATEWAY 2

// Include semua sub-konfigurasi
#include "config/credentials.h"
#include "config/hardware.h"
#include "config/features.h"
#include "config/tuning.h"