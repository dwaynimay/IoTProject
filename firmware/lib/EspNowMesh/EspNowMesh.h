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

// Queue global — didefinisikan di EspNowMesh.cpp
// Dibuat di main.cpp sebelum mesh.begin() dipanggil
extern QueueHandle_t g_rawQueue;
extern QueueHandle_t g_mqttQueue;


// =============================================================================
// EspNowMesh
// =============================================================================
class EspNowMesh
{
public:
    EspNowMesh() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Inisialisasi ESP-NOW.
    // senderMode = true  → sensor node (tambah peer gateway, siap kirim)
    // senderMode = false → gateway node (daftarkan semua peer sensor, siap terima)
    // Kembalikan true jika inisialisasi berhasil.
    bool begin(bool senderMode);

    // ── Send API (Sensor Node) ────────────────────────────────────────────────

    // Kirim satu axis CS ke gateway.
    // pktType: PKT_CS_AX, PKT_CS_AY, ..., PKT_CS_GZ
    bool sendCsAxis(uint8_t pktType, uint8_t nodeId,
                    const float y[CS_M], bool fingerOn,
                    uint32_t timestamp);

    // Kirim data PPG CS ke gateway.
    bool sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                   int8_t heartRate, bool ppgValid,
                   bool fingerOn, uint32_t timestamp);

    // Kirim CombinedPacket (mode non-CS).
    bool sendCombined(const CombinedPacket& pkt);

    // Kirim heartbeat periodik.
    bool sendHeartbeat(uint8_t nodeId, uint32_t uptimeS);

    // ── Status ────────────────────────────────────────────────────────────────

    bool lastSendOk() const { return _lastSendOk; }

private:
    bool _senderMode = true;
    bool _lastSendOk = false;

    // Helper: kirim buffer ke MAC gateway via ESP-NOW
    bool _send(const void* data, size_t len);

    // Helper: daftarkan satu peer ke ESP-NOW
    bool _addPeer(const uint8_t* mac);

    // ── ESP-NOW Callbacks (harus static — C-style function pointer) ───────────

    static void _onDataSent(const uint8_t* mac, esp_now_send_status_t status);

    // ISR: HANYA memcpy ke g_rawQueue — tidak ada logika lain!
    static void _onDataRecv(const uint8_t* mac, const uint8_t* data, int len);

    // Pointer ke instance aktif untuk callback (singleton pattern)
    static EspNowMesh* _instance;
};