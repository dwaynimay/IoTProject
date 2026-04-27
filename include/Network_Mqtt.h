#pragma once
// =============================================================================
// Network_Mqtt.h — MQTT Client (Hanya untuk Node Gateway)
// Menggunakan library: knolleary/PubSubClient
//
// WiFi dan MQTT dihandle di dalam class ini.
// Task MQTT membaca dari g_mqttQueue dan publish ke broker.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "DataModels.h"
#include "Config.h"

class NetworkMqtt {
public:
    NetworkMqtt() : _client(_wifiClient) {}

    // Konek ke WiFi lalu ke MQTT broker.
    // Blokir sampai berhasil atau timeout (Timing::WIFI_TIMEOUT_MS).
    bool begin();

    // Publish satu pesan. Reconnect otomatis jika terputus.
    // Dipanggil dari FreeRTOS task, aman karena sudah ada mutex.
    bool publish(const char* topic, const char* payload, bool retain = false);

    // Loop MQTT (panggil di akhir setiap iterasi task agar keepalive bekerja)
    void loop();

    bool isConnected() const { return _client.connected(); }
    bool isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }

private:
    WiFiClient   _wifiClient;
    PubSubClient _client;

    bool connectWifi();
    bool connectMqtt();

    uint32_t _lastReconnectAttempt = 0;
};