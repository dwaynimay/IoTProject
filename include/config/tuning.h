#pragma once
// =============================================================================
// config/tuning.h — PARAMETER PERFORMA & TIMING
// =============================================================================
// File ini untuk pengguna yang ingin menyetel performa sistem.
// Penggunaan normal tidak perlu mengubah file ini.
//
// ⚠️  Perubahan di sini bisa memengaruhi stabilitas — ubah satu per satu
//     dan test setelah setiap perubahan.
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// SEND INTERVAL — Seberapa sering sensor mengirim data ke gateway
//
//   100ms = 10 Hz  → resolusi tinggi, cocok untuk deteksi gerakan/gesture
//   200ms =  5 Hz  → balance antara detail dan beban jaringan (default)
//   500ms =  2 Hz  → monitoring santai (postur, SpO2 jangka panjang)
//
// Nilai ini juga mempengaruhi latensi MQTT Batching jika diaktifkan.
// ---------------------------------------------------------------------------
namespace Timing
{
    constexpr uint32_t SEND_INTERVAL_MS  = 200;   // ← UBAH INI untuk kontrol kecepatan kirim

    // Internal — jangan diubah kecuali ada alasan hardware
    constexpr uint32_t PPG_POLL_MS       = 0;     // PPG polling secepat mungkin
    constexpr uint32_t IMU_SAMPLE_MS     = 10;    // 100 Hz internal IMU
    constexpr uint32_t MQTT_PUBLISH_MS   = 500;   // timeout tunggu queue
    constexpr uint32_t WIFI_TIMEOUT_MS   = 10000; // batas waktu konek WiFi
    constexpr uint32_t HEARTBEAT_MS      = 30000; // interval heartbeat node → gateway
}

// ---------------------------------------------------------------------------
// WATCHDOG THRESHOLD
//
// MIN_FREE_HEAP_KB: restart otomatis jika heap di bawah nilai ini.
//   Gateway normal: 8–12 KB tersisa (WiFi + MQTT + ESP-NOW makan banyak RAM)
//   Sensor normal: 50–80 KB tersisa
//
// Threshold sudah disesuaikan per-role di Watchdog.h — parameter ini
// hanya untuk referensi dokumentasi.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FREERTOS TASK PRIORITY (1 = terendah, 24 = tertinggi di ESP32)
//
// Urutan prioritas saat ini:
//   PPG (4) > IMU (3) > ESPNOW_TX / MQTT_PUB (2) > MONITOR (1)
//
// PPG butuh polling cepat agar tidak kehilangan beat → prioritas tertinggi.
// Jangan set semua ke prioritas sama — FreeRTOS butuh hierarki.
// ---------------------------------------------------------------------------
namespace TaskPrio
{
    constexpr uint8_t SENSOR_PPG = 4;
    constexpr uint8_t SENSOR_IMU = 3;
    constexpr uint8_t ESPNOW_TX  = 2;
    constexpr uint8_t MQTT_PUB   = 2;
    constexpr uint8_t MONITOR    = 1;
}

// ---------------------------------------------------------------------------
// FREERTOS STACK SIZE (bytes per task)
//
// CS_TX lebih besar karena:
//   - 7 array float[32] lokal di stack = 896 byte
//   - Matrix Φ encode overhead
//   - snprintf untuk log
//
// Naikkan jika ada stack overflow (terlihat di Serial: "[WDT] Stack kritis")
// ---------------------------------------------------------------------------
namespace StackSize
{
    constexpr uint32_t SENSOR_PPG = 4096;
    constexpr uint32_t SENSOR_IMU = 4096;
    constexpr uint32_t ESPNOW_TX  = 12288; // CS TX butuh stack besar
    constexpr uint32_t MQTT_PUB   = 8192;
    constexpr uint32_t MONITOR    = 4096;
}

// ---------------------------------------------------------------------------
// QUEUE SIZE
//
// MQTT_MSG: jumlah pesan yang bisa antri sebelum gateway mulai buang paket.
// RAM yang dipakai: sizeof(MqttMessage) × MQTT_MSG = 500B × 30 = 15 KB
//
// Jangan naikkan sembarangan — setiap +10 entry = +5 KB heap gateway.
// Cek log "[MONITOR] Queue MQTT: X% penuh" untuk tahu apakah perlu dinaikkan.
// ---------------------------------------------------------------------------
namespace QueueLen
{
    constexpr uint8_t IMU_DATA = 1;
    constexpr uint8_t PPG_DATA = 1;
    constexpr uint8_t MQTT_MSG = 30;
}
