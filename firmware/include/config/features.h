// File: firmware/include/config/features.h

#pragma once
// =============================================================================
// features.h — FITUR ON/OFF & KONSTANTA OPERASIONAL
// =============================================================================
//
// Catatan namespace Mqtt:
//   credentials.h  → BROKER, PORT, CLIENT_ID, USER, PASSWORD
//   features.h     → TOPIC_BASE, KEEPALIVE, RECONNECT_DELAY_MS
// =============================================================================


// ---------------------------------------------------------------------------
// DEBUG & LOGGING
//
// LOG_LEVEL:
//   0 = SILENT  — tidak ada output
//   1 = ERROR   — kondisi fatal saja
//   2 = WARN    — peringatan + error           ← PRODUKSI (kurangi Serial blocking)
//   3 = INFO    — informasi umum               ← DEBUGGING (banyak output)
//   4 = DEBUG   — semua pesan termasuk detail internal
//
// Serial.printf() di ESP32 bersifat BLOCKING (~0.5–2ms per baris).
// Di LOG_LEVEL=3: ~50 calls/detik → overhead CPU signifikan.
// Di LOG_LEVEL=2: hanya WARN/ERROR → hampir tidak ada Serial overhead.
// ---------------------------------------------------------------------------
#define LOG_LEVEL        3  
#define LOG_ENABLE_COLOR 0


// ---------------------------------------------------------------------------
// SENSOR LIMITS & CONFIG
// ---------------------------------------------------------------------------
namespace EdgeConfig
{
    constexpr uint32_t IR_FINGER_THRESHOLD = 50000;
}


// ---------------------------------------------------------------------------
// MQTT — Konstanta operasional
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