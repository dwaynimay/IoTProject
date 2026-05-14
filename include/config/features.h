#pragma once
// =============================================================================
// config/features.h — FITUR ON/OFF
// =============================================================================
// File ini untuk mengaktifkan atau menonaktifkan fitur tanpa harus
// memahami kode. Cukup ganti true/false lalu compile ulang.
// =============================================================================

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
    constexpr bool ENABLE_FINGER_GATE = false;

    // Nilai IR minimum agar dianggap "ada jari"
    // Naikkan jika sering false-positive, turunkan jika sulit terdeteksi
    // Rentang normal: 50.000 – 100.000
    constexpr uint32_t IR_FINGER_THRESHOLD = 50000;
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
//
// Topik lengkap per node:
//   {TOPIC_BASE}/node_1/combined   → data IMU + PPG mentah
//   {TOPIC_BASE}/node_1/cs_ax      → compressive sensing axis X
//   {TOPIC_BASE}/node_1/heartbeat  → status node hidup
//   {TOPIC_BASE}/gateway/status    → status gateway online/offline
// ---------------------------------------------------------------------------
namespace Mqtt
{
    constexpr char     TOPIC_BASE[]        = "health_monitor";
    constexpr uint16_t KEEPALIVE           = 60;   // detik
    constexpr uint16_t RECONNECT_DELAY_MS  = 5000; // ms sebelum retry koneksi
}
