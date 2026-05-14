#pragma once
// =============================================================================
// Network_EspNow.h — Abstraksi ESP-NOW (Peer-to-Peer)
//
// Arsitektur:
//   Sensor Node (A/B) → kirim CombinedPacket ke Gateway setiap SEND_INTERVAL_MS
//   Gateway Node (C)  → terima dari A & B, routing ke queue MQTT per node_id
//
// [Item #5 ISR Offload] Pipeline baru di gateway:
//
//   onDataRecv ISR          taskSerialize           taskMqttPublish
//   ──────────────          ─────────────           ───────────────
//   memcpy raw bytes   →    format JSON        →    mqtt.publish()
//   xQueueSendFromISR        xQueueReceive           xQueueReceive
//   ~5µs (aman di ISR)       boleh lambat            tidak berubah
//        ↓                        ↓
//    g_rawQueue             g_mqttQueue
//
// Sebelumnya onDataRecv melakukan snprintf 32× (CS_M) dan Serial.printf
// langsung di ISR → bisa makan beberapa ms → watchdog timeout di gateway.
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "DataModels.h"
#include "Config.h"

// ---------------------------------------------------------------------------
// Queue global
//
// g_rawQueue  — ISR → taskSerialize (raw ESP-NOW bytes, isi RawPacket)
//               Didefinisikan di Network_EspNow.cpp
//               Dibuat di main.cpp sebelum g_espnow.begin()
//
// g_mqttQueue — taskSerialize → taskMqttPublish (JSON siap publish)
//               Didefinisikan di Network_EspNow.cpp
//               Dibuat di main.cpp (tidak berubah dari sebelumnya)
// ---------------------------------------------------------------------------
extern QueueHandle_t g_rawQueue;
extern QueueHandle_t g_mqttQueue;

class NetworkEspNow {
public:
    NetworkEspNow() = default;

    // Inisialisasi ESP-NOW.
    // senderMode = true  → tambahkan peer gateway, siap kirim
    // senderMode = false → siap terima (gateway), daftarkan semua peer sensor
    bool begin(bool senderMode);

    // --- Sensor Node API ---

    // Kirim CombinedPacket (IMU + PPG + EdgeResult) ke gateway.
    bool sendCombined(const CombinedPacket& pkt);

    // Kirim heartbeat periodik
    bool sendHeartbeat(const HeartbeatPacket& pkt);

    // Cek apakah pengiriman terakhir berhasil (diupdate di callback)
    bool lastSendOk() const { return _lastSendOk; }

private:
    bool _senderMode  = true;
    bool _lastSendOk  = false;

    // Callback statis (ESP-NOW hanya menerima C-style function pointer)
    static void onDataSent(const uint8_t* mac, esp_now_send_status_t status);

    // [Item #5] ISR minimal — hanya memcpy ke g_rawQueue
    // Semua serialisasi JSON dipindah ke taskSerialize di main.cpp
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);

    // Helper: daftarkan satu peer
    bool addPeer(const uint8_t* mac);
};