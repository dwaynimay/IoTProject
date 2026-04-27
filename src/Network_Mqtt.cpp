// =============================================================================
// Network_Mqtt.cpp — Implementasi MQTT Client
// =============================================================================

#include "Network_Mqtt.h"

// ---------------------------------------------------------------------------
bool NetworkMqtt::begin() {
    _client.setServer(Mqtt::BROKER, Mqtt::PORT);
    _client.setKeepAlive(Mqtt::KEEPALIVE);
    _client.setBufferSize(512); // payload buffer — sesuaikan jika perlu

    if (!connectWifi()) {
        Serial.println("[MQTT] ERROR: WiFi gagal tersambung!");
        return false;
    }

    if (!connectMqtt()) {
        Serial.println("[MQTT] WARN: MQTT gagal tersambung, akan retry di loop.");
    }

    return true;
}

// ---------------------------------------------------------------------------
bool NetworkMqtt::connectWifi() {
    Serial.printf("[WiFi] Menghubungkan ke '%s'", Wifi::SSID);
    WiFi.begin(Wifi::SSID, Wifi::PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > Timing::WIFI_TIMEOUT_MS) {
            Serial.println(" TIMEOUT!");
            return false;
        }
        delay(500);
        Serial.print(".");
    }

    Serial.printf("\n[WiFi] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// ---------------------------------------------------------------------------
bool NetworkMqtt::connectMqtt() {
    if (WiFi.status() != WL_CONNECTED) {
        connectWifi(); // coba reconnect WiFi dulu
    }

    Serial.printf("[MQTT] Menghubungkan ke %s:%d ...", Mqtt::BROKER, Mqtt::PORT);

    bool ok;
    if (strlen(Mqtt::USER) > 0) {
        ok = _client.connect(Mqtt::CLIENT_ID, Mqtt::USER, Mqtt::PASSWORD);
    } else {
        ok = _client.connect(Mqtt::CLIENT_ID);
    }

    if (ok) {
        Serial.println(" OK");
        // Publish status online ke LWT topic
        char lwt[64];
        snprintf(lwt, sizeof(lwt), "%s/gateway/status", Mqtt::TOPIC_BASE);
        _client.publish(lwt, "online", true);
    } else {
        Serial.printf(" GAGAL (rc=%d)\n", _client.state());
    }

    return ok;
}

// ---------------------------------------------------------------------------
bool NetworkMqtt::publish(const char* topic, const char* payload, bool retain) {
    // Reconnect jika perlu (non-blokir dengan debounce)
    if (!_client.connected()) {
        uint32_t now = millis();
        if (now - _lastReconnectAttempt > Timing::RECONNECT_DELAY_MS) {
            _lastReconnectAttempt = now;
            connectMqtt();
        }
        if (!_client.connected()) return false;
    }

    bool ok = _client.publish(topic, payload, retain);
    if (!ok) {
        Serial.printf("[MQTT] WARN: publish gagal ke '%s'\n", topic);
    }
    return ok;
}

// ---------------------------------------------------------------------------
void NetworkMqtt::loop() {
    _client.loop();
}