// File: firmware/lib/EspNowMesh/EspNowMesh.h

#pragma once
// =============================================================================
// EspNowMesh.h — Abstraksi Transport Layer ESP-NOW
// =============================================================================
//
// Tanggung jawab modul ini:
//   1. Inisialisasi ESP-NOW (channel, peer registration)
//   2. Kirim packet dari sensor ke gateway
//   3. ISR penerima di gateway — MINIMAL, hanya memcpy ke queue
//
// Apa yang BUKAN tanggung jawab modul ini:
//   - Serialisasi JSON           → MeshRouting
//   - Logika bisnis packet       → MeshRouting
//   - Publish ke MQTT            → Network_Mqtt
//
// ARSITEKTUR ISR (Gateway):
//
//   [ESP-NOW onDataRecv — WiFi task context]
//         ↓ memcpy ~1µs (aman)
//   [g_rawQueue]
//         ↓ xQueueReceive
//   [taskMeshHandler — src/task_mesh_handler.cpp]
//         ↓ MeshRouting::route()
//   [g_mqttQueue]
//         ↓ xQueueReceive
//   [taskMqttPublish — src/main.cpp]
//
// Kenapa ISR harus minimal?
//   onDataRecv berjalan di WiFi task context (setara ISR priority).
//   snprintf + Serial.printf di dalamnya bisa makan 1–5ms →
//   watchdog idle task timeout → gateway restart loop.
//   memcpy 250 bytes ~1µs → aman.
//
// CARA PAKAI (Sensor Node):
//   EspNowMesh mesh;
//   mesh.begin(true);                 // true = sender mode
//   mesh.sendCsAxis(PKT_CS_AX, ...);
//   mesh.sendCsPpg(...);
//   mesh.sendHeartbeat(...);
//
// CARA PAKAI (Gateway Node):
//   EspNowMesh mesh;
//   g_rawQueue = xQueueCreate(10, sizeof(RawPacket)); // buat dulu!
//   mesh.begin(false);                // false = receiver mode
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "MeshPackets.h"
#include "../../include/Config.h"

extern QueueHandle_t g_rawQueue;
extern QueueHandle_t g_mqttQueue;
extern volatile uint64_t g_epochOffsetMs;


class EspNowMesh
{
public:
    EspNowMesh() = default;

    bool begin(bool senderMode);

    // ── Send API (Sensor Node) ────────────────────────────────────────────────

    bool sendCsAxis(uint8_t pktType, uint8_t nodeId,
                    const float y[CS_M], bool fingerOn,
                    uint32_t timestamp);

    // spo2: nilai SpO2 dalam % dari SensorPPG, atau 0.0 jika tidak valid
    bool sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                   int8_t heartRate, bool ppgValid,
                   float spo2,        // ← parameter baru
                   bool fingerOn, uint32_t timestamp);

    bool sendCombined(const CombinedPacket& pkt);

    bool sendHeartbeat(uint8_t nodeId, uint32_t uptimeS);

    bool sendTimeSync(uint32_t epochS, uint16_t epochMsPart);

    bool lastSendOk() const { return _lastSendOk; }

private:
    bool _senderMode = true;
    bool _lastSendOk = false;

    bool _send(const void* data, size_t len);
    bool _addPeer(const uint8_t* mac);

    static void _onDataSent(const uint8_t* mac, esp_now_send_status_t status);
    static void _onDataRecv(const uint8_t* mac, const uint8_t* data, int len);

    static EspNowMesh* _instance;
};