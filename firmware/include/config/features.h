// File: firmware/include/config/features.h

#pragma once
// =============================================================================
// config/features.h — FITUR ON/OFF
// =============================================================================
// Ubah nilai true/false di sini lalu compile ulang.
// Tidak perlu menyentuh file lain untuk toggle fitur.
// =============================================================================


// ---------------------------------------------------------------------------
// DEBUG & LOGGING
//
// LOG_LEVEL mengontrol seberapa banyak pesan yang dicetak ke Serial.
// Pilihan (dari paling senyap ke paling bising):
//   0 = SILENT  — tidak ada output sama sekali
//   1 = ERROR   — hanya kondisi fatal
//   2 = WARN    — peringatan + error
//   3 = INFO    — informasi umum + warn + error   ← production recommended
//   4 = DEBUG   — semua pesan, termasuk detail internal
//
// Tip: Set ke 4 (DEBUG) saat development, turunkan ke 3 (INFO) saat deploy.
//
// LOG_ENABLE_COLOR: warnai output Serial berdasarkan level.
//   Sangat membantu saat baca log di Serial Monitor Arduino/PlatformIO.
//   Nonaktifkan jika terminal kamu tidak support ANSI color codes.
// ---------------------------------------------------------------------------
#define LOG_LEVEL        4     // ← ubah ini untuk kontrol verbosity
#define LOG_ENABLE_COLOR 1     // 1 = aktif, 0 = nonaktif


// ---------------------------------------------------------------------------
// FINGER GATE — Blokir pengiriman data jika jari tidak menempel ke sensor
//
//   true  → data HANYA dikirim saat jari terdeteksi di MAX30102
//             (hemat bandwidth, cocok untuk deployment nyata)
//   false → data selalu dikirim meski tidak ada jari
//             (bagus untuk debug & kalibrasi awal)
// ---------------------------------------------------------------------------
namespace EdgeConfig
{
    constexpr bool     ENABLE_FINGER_GATE   = false;
    constexpr uint32_t IR_FINGER_THRESHOLD  = 50000;
}


// ---------------------------------------------------------------------------
// MQTT BATCHING — Kumpulkan beberapa sampel sebelum dikirim ke broker
//
//   false → setiap paket langsung dikirim (latensi rendah, cocok real-time)
//   true  → kumpulkan BATCH_SIZE sampel, kirim 1 kali (hemat publish call)
//
// ⚠️  Batching menambah latensi sebesar: BATCH_SIZE × SEND_INTERVAL_MS
//     Contoh: 5 × 200ms = 1 detik latensi tambahan
// ---------------------------------------------------------------------------
namespace BatchConfig
{
    constexpr bool    BATCHING_ENABLED = false;
    constexpr uint8_t BATCH_SIZE       = 5;
}


// ---------------------------------------------------------------------------
// MQTT TOPICS — Format topik yang dipublish ke broker
// ---------------------------------------------------------------------------
namespace Mqtt
{
    constexpr char     TOPIC_BASE[]       = "health_monitor";
    constexpr uint16_t KEEPALIVE          = 60;
    constexpr uint16_t RECONNECT_DELAY_MS = 5000;
}