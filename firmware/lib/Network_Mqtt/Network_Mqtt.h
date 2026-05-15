// File: firmware/lib/Network_Mqtt/Network_Mqtt.h

#pragma once
// =============================================================================
// Network_Mqtt.h — MQTT Client over WiFi (Gateway Node Only)
// =============================================================================
//
// Tanggung jawab modul ini:
//   1. Koneksi WiFi dalam mode WIFI_AP_STA
//   2. Koneksi MQTT ke broker (PubSubClient)
//   3. Auto-reconnect dengan exponential backoff
//   4. Publish pesan ke topic MQTT
//
// Kenapa WIFI_AP_STA bukan pure STA?
//   Gateway sebagai pure STA akan melakukan "channel hopping" saat idle.
//   Channel yang berubah-ubah membuat paket ESP-NOW dari sensor sering
//   tidak tertangkap (NACK di sisi sensor).
//   Dengan AP aktif (meski tersembunyi), channel radio dikunci → ESP-NOW stabil.
//
// CARA PAKAI:
//   NetworkMqtt mqtt;
//   mqtt.begin();                              // koneksi WiFi + MQTT
//   mqtt.publish("topic/sensor", "{...}");     // publish pesan
//   mqtt.loop();                               // panggil di setiap iterasi task
//
// RECONNECT STRATEGY:
//   Reconnect tidak dilakukan di dalam publish() atau loop() secara blocking.
//   Gunakan isConnected() untuk cek status, dan begin() untuk reconnect manual
//   jika diperlukan dari task monitor.
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