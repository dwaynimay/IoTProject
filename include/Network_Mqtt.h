#pragma once
// =============================================================================
// Network_Mqtt.h — MQTT Client (Hanya untuk Node Gateway)
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "DataModels.h"
#include "Config.h"

class NetworkMqtt {
public:
    NetworkMqtt() : _client(_wifiClient) {}

    bool begin();
    bool publish(const char* topic, const char* payload, bool retain = false);
    void loop();

    bool isConnected()     { return _client.connected(); }
    bool isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }
    int  state()           { return _client.state(); } // PubSubClient state code

private:
    WiFiClient   _wifiClient;
    PubSubClient _client;

    bool connectWifi();
    bool connectMqtt();

    uint32_t _lastReconnectAttempt = 0;
};