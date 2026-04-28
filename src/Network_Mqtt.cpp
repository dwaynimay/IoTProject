// =============================================================================
// Network_Mqtt.cpp — MQTT Client (Gateway Node)
//
// Gateway menggunakan WIFI_AP_STA mode:
//   - STA: konek ke router → dapat IP → bisa MQTT
//   - AP:  mengunci channel radio agar ESP-NOW stabil (AP tidak perlu dipakai)
//
// Tanpa AP, gateway sebagai pure STA akan "channel hopping" saat idle
// sehingga paket ESP-NOW dari sensor sering miss → NACK di sensor.
// =============================================================================

#include "Network_Mqtt.h"
#include <esp_wifi.h>

bool NetworkMqtt::begin() {
    _client.setServer(Mqtt::BROKER, Mqtt::PORT);
    _client.setKeepAlive(Mqtt::KEEPALIVE);
    _client.setBufferSize(512);

    if (!connectWifi()) {
        Serial.println("[MQTT] ERROR: WiFi gagal tersambung!");
        return false;
    }

    if (!connectMqtt()) {
        Serial.println("[MQTT] WARN: MQTT gagal tersambung, akan retry di loop.");
    }

    return true;
}

bool NetworkMqtt::connectWifi() {
    // WIFI_AP_STA: STA untuk konek router, AP untuk kunci channel ESP-NOW
    // AP tidak perlu diakses siapapun — hanya untuk stabilkan channel
    WiFi.mode(WIFI_AP_STA);

    // Mulai AP dulu di channel yang sama dengan router (channel 1)
    // Password minimal 8 karakter agar AP valid, SSID boleh tersembunyi
    WiFi.softAP("gw_espnow_lock", "12345678", 1 /*channel*/, 1 /*hidden*/);
    delay(100);

    // Konek ke router sebagai STA
    Serial.printf("[WiFi] Konek ke '%s'", Wifi::SSID);
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

    // Verifikasi channel setelah konek
    uint8_t ch; wifi_second_chan_t sch;
    esp_wifi_get_channel(&ch, &sch);
    Serial.printf("[WiFi] Channel aktual: %d\n", ch);

    return true;
}

bool NetworkMqtt::connectMqtt() {
    if (WiFi.status() != WL_CONNECTED) connectWifi();

    Serial.printf("[MQTT] Konek ke %s:%d ...", Mqtt::BROKER, Mqtt::PORT);

    bool ok = (strlen(Mqtt::USER) > 0)
        ? _client.connect(Mqtt::CLIENT_ID, Mqtt::USER, Mqtt::PASSWORD)
        : _client.connect(Mqtt::CLIENT_ID);

    if (ok) {
        Serial.println(" OK");
        char lwt[64];
        snprintf(lwt, sizeof(lwt), "%s/gateway/status", Mqtt::TOPIC_BASE);
        _client.publish(lwt, "online", true);
    } else {
        Serial.printf(" GAGAL (rc=%d)\n", _client.state());
    }
    return ok;
}

bool NetworkMqtt::publish(const char* topic, const char* payload, bool retain) {
    if (!_client.connected()) {
        uint32_t now = millis();
        if (now - _lastReconnectAttempt > Mqtt::RECONNECT_DELAY_MS) {
            _lastReconnectAttempt = now;
            connectMqtt();
        }
        if (!_client.connected()) return false;
    }
    bool ok = _client.publish(topic, payload, retain);
    if (!ok) Serial.printf("[MQTT] WARN: publish gagal ke '%s'\n", topic);
    return ok;
}

void NetworkMqtt::loop() {
    _client.loop();
}