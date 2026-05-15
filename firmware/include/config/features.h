// File: firmware/include/config/features.h

#pragma once
// =============================================================================
// config/features.h — FITUR ON/OFF & KONSTANTA OPERASIONAL
// =============================================================================
// Ubah nilai true/false di sini lalu compile ulang.
//
// Catatan namespace Mqtt:
//   credentials.h  → BROKER, PORT, CLIENT_ID, USER, PASSWORD  (sensitif)
//   features.h     → TOPIC_BASE, KEEPALIVE, RECONNECT_DELAY_MS (operasional)
// Keduanya di-include oleh Config.h sehingga namespace Mqtt tergabung otomatis.
// =============================================================================


// ---------------------------------------------------------------------------
// DEBUG & LOGGING
//
// LOG_LEVEL:
//   0 = SILENT  — tidak ada output
//   1 = ERROR   — kondisi fatal saja
//   2 = WARN    — peringatan + error
//   3 = INFO    — informasi umum (production recommended)
//   4 = DEBUG   — semua pesan termasuk detail internal
// ---------------------------------------------------------------------------
#define LOG_LEVEL        4
#define LOG_ENABLE_COLOR 1


// ---------------------------------------------------------------------------
// FINGER GATE — Blokir pengiriman data jika jari tidak menempel
// ---------------------------------------------------------------------------
namespace EdgeConfig
{
    constexpr bool     ENABLE_FINGER_GATE  = false;
    constexpr uint32_t IR_FINGER_THRESHOLD = 50000;
}


// ---------------------------------------------------------------------------
// MQTT BATCHING
// ---------------------------------------------------------------------------
namespace BatchConfig
{
    constexpr bool    BATCHING_ENABLED = false;
    constexpr uint8_t BATCH_SIZE       = 5;
}


// ---------------------------------------------------------------------------
// MQTT — Konstanta operasional (bukan kredensial)
//
// Network_Mqtt.cpp membutuhkan:
//   Mqtt::TOPIC_BASE        → prefix topic MQTT
//   Mqtt::KEEPALIVE         → interval keepalive (detik)
//   Mqtt::RECONNECT_DELAY_MS→ delay awal reconnect (ms), naik exponential
//
// credentials.h menyediakan: BROKER, PORT, CLIENT_ID, USER, PASSWORD
// ---------------------------------------------------------------------------
namespace Mqtt
{
    constexpr char     TOPIC_BASE[]        = "health_monitor";
    constexpr uint16_t KEEPALIVE           = 60;
    constexpr uint16_t RECONNECT_DELAY_MS  = 5000;
}