// File: firmware/lib/Network_Mqtt/Network_Mqtt.h

#pragma once
// =============================================================================
// NetworkMqtt — MQTT Client over WiFi (Gateway Node Only)
// =============================================================================
//
// Hardware  : ESP32 WiFi radio
// Why this implementation:
//             Manages WiFi connection in WIFI_AP_STA mode alongside MQTT broker
//             communications with exponential backoff reconnect logic.
//             - WIFI_AP_STA mode is used instead of pure STA to activate a hidden
//               AP, locking the radio channel. This prevents gateway channel
//               hopping during STA idle periods, which would otherwise lead to
//               missed ESP-NOW packets from sensor nodes.
//
// USAGE:
//   NetworkMqtt mqtt;
//   mqtt.begin();
//   mqtt.publish("topic/sensor", "payload");
//   mqtt.loop();
//
// THREAD SAFETY:
//   Not thread-safe. Reconnection attempts and data publishes should only be
//   performed from a single network task (typically taskMqttClient).
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "MeshPackets.h"
#include "../../include/Config.h"


// =============================================================================
// NetworkMqtt
// =============================================================================
class NetworkMqtt
{
public:
    NetworkMqtt() : _mqttClient(_wifiClient) {}

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Hubungkan WiFi lalu MQTT broker.
    // Kembalikan true jika WiFi berhasil (MQTT boleh gagal — akan retry di loop).
    // Harus dipanggil SETELAH esp_now_init() selesai.
    bool begin();

    // ── Publish API ───────────────────────────────────────────────────────────

    // Publish payload ke topic.
    // Kembalikan false jika tidak terhubung atau publish gagal.
    // Tidak blocking — jika MQTT sedang disconnect, langsung return false.
    bool publish(const char* topic, const char* payload, bool retain = false);

    // ── Maintenance ───────────────────────────────────────────────────────────

    // Init NTP setelah WiFi connect
    void initNTP();

    // Helper untuk waktu
    uint32_t getEpochS() const;
    uint16_t getEpochMsPart() const;

    // Proses incoming message dan jaga koneksi MQTT tetap hidup.
    // Panggil di setiap iterasi task MQTT.
    void loop();

    // Coba reconnect ke MQTT broker jika koneksi putus.
    // Non-blocking: hanya mencoba jika sudah lewat interval backoff.
    // Kembalikan true jika berhasil terhubung kembali.
    bool tryReconnect();

    // ── Status ────────────────────────────────────────────────────────────────

    bool     isConnected()      { return _mqttClient.connected(); }
    bool     isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }
    int      mqttState()        { return _mqttClient.state(); }
    uint32_t publishCount()    const { return _publishCount; }
    uint32_t failCount()       const { return _failCount; }

private:
    WiFiClient   _wifiClient;
    PubSubClient _mqttClient;

    uint32_t _publishCount         = 0;
    uint32_t _failCount            = 0;
    uint32_t _lastReconnectAttempt = 0;

    // Exponential backoff untuk reconnect:
    //   Percobaan pertama: RECONNECT_BASE_MS
    //   Percobaan berikutnya: min(delay × 2, RECONNECT_MAX_MS)
    uint32_t _reconnectDelay = Mqtt::RECONNECT_DELAY_MS;

    static constexpr uint32_t RECONNECT_MAX_MS = 60000; // maksimum 60 detik

    // ── Private Helpers ───────────────────────────────────────────────────────

    bool _connectWifi();
    bool _connectMqtt();

    // Publish Last Will Testament — status gateway "offline" saat koneksi putus
    void _publishOnlineStatus();

    // Teks deskriptif untuk PubSubClient state code (untuk logging)
    static const char* _mqttStateStr(int state);
};