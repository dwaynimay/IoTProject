#pragma once
// =============================================================================
// Network_EspNow.h — Abstraksi ESP-NOW (Peer-to-Peer Mesh)
//
// Arsitektur:
//   Sensor Node (A/B) → hanya mengirim ke Gateway
//   Gateway Node (C)  → menerima dari A & B, memasukkan ke queue MQTT
//
// Callback ESP-NOW berjalan di konteks ISR/WiFi task, bukan FreeRTOS task —
// sehingga pengolahan berat harus di-delegate ke queue.
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "DataModels.h"
#include "Config.h"

// Queue global untuk data yang diterima gateway (dideklarasikan di .cpp)
extern QueueHandle_t g_mqttQueue;

class NetworkEspNow {
public:
    NetworkEspNow() = default;

    // Inisialisasi ESP-NOW.
    // senderMode = true  → tambahkan peer gateway, siap kirim
    // senderMode = false → siap terima (gateway), daftarkan semua peer sensor
    bool begin(bool senderMode);

    // Kirim data IMU ke gateway. Kembalikan true jika esp_now_send berhasil.
    bool sendImu(const ImuPacket& pkt);

    // Kirim data PPG ke gateway.
    bool sendPpg(const PpgPacket& pkt);

    // Kirim heartbeat.
    bool sendHeartbeat(const HeartbeatPacket& pkt);

    // Cek apakah pengiriman terakhir berhasil (diupdate di callback)
    bool lastSendOk() const { return _lastSendOk; }

private:
    bool _senderMode = true;
    bool _lastSendOk = false;

    // Callback statis (ESP-NOW hanya menerima C-style function pointer)
    static void onDataSent(const uint8_t* mac, esp_now_send_status_t status);
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);

    // Helper: daftarkan satu peer
    bool addPeer(const uint8_t* mac);
};