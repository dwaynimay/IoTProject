// File: firmware/lib/Network_Mqtt/Network_Mqtt.cpp

// =============================================================================
// Network_Mqtt.cpp — Implementasi MQTT Client
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "Network_Mqtt.h"
#include <esp_wifi.h>
#include <time.h>
#include <sys/time.h>

static constexpr char TAG[] = "MQTT";


// =============================================================================
// begin() — Inisialisasi WiFi dan MQTT
// =============================================================================
bool NetworkMqtt::begin()
{
    _mqttClient.setServer(Mqtt::BROKER, Mqtt::PORT);
    _mqttClient.setKeepAlive(Mqtt::KEEPALIVE);

    // Buffer 1200 byte — cukup untuk payload CS (~900 byte) + margin
    _mqttClient.setBufferSize(1900);

    if (!_connectWifi())
    {
        LOG_ERROR(TAG, "WiFi gagal tersambung — gateway tidak bisa beroperasi");
        return false;
    }

    // Init NTP setelah WiFi terhubung
    initNTP();

    // MQTT boleh gagal saat pertama kali — tryReconnect() akan handle ini
    if (!_connectMqtt())
    {
        LOG_WARN(TAG, "MQTT belum terhubung saat init — akan retry otomatis");
    }

    return true; // WiFi OK sudah cukup untuk begin() dianggap berhasil
}


// =============================================================================
// publish() — Kirim Pesan ke Broker
// =============================================================================
bool NetworkMqtt::publish(const char* topic, const char* payload, bool retain)
{
    if (!_mqttClient.connected())
    {
        _failCount++;
        LOG_EVERY_N(10, LOG_WARN, TAG, "Publish gagal — MQTT tidak terhubung (fail #%lu)",
                    _failCount);
        return false;
    }

    // Cek ukuran sebelum kirim
    size_t payloadLen = strlen(payload);
    size_t topicLen   = strlen(topic);
    
    if (payloadLen + topicLen + 5 > 1900) // 5 = MQTT fixed header overhead
    {
        LOG_WARN(TAG, "Payload terlalu besar: topic=%d payload=%d total=%d bytes",
                 topicLen, payloadLen, topicLen + payloadLen + 5);
        _failCount++;
        return false;
    }

    const bool ok = _mqttClient.publish(topic, payload, retain);

    if (ok)
    {
        _publishCount++;
        LOG_DEBUG(TAG, "#%lu → %s (%d bytes)", _publishCount, topic, strlen(payload));
    }
    else
    {
        _failCount++;
        LOG_WARN(TAG, "Publish gagal ke '%s' | rc=%d (fail #%lu)",
                 topic, _mqttClient.state(), _failCount);
    }

    return ok;
}


// =============================================================================
// loop() — Maintenance Koneksi
// =============================================================================
void NetworkMqtt::loop()
{
    _mqttClient.loop();
}


// =============================================================================
// tryReconnect() — Reconnect dengan Exponential Backoff
//
// Pola exponential backoff mencegah gateway membanjiri broker dengan
// request reconnect saat broker sedang down atau overload.
//
// Contoh timeline backoff (base=5s, max=60s):
//   Gagal #1 → tunggu 5s
//   Gagal #2 → tunggu 10s
//   Gagal #3 → tunggu 20s
//   Gagal #4 → tunggu 40s
//   Gagal #5+ → tunggu 60s (cap)
// =============================================================================
bool NetworkMqtt::tryReconnect()
{
    const uint32_t now = millis();

    // Belum waktunya retry — return cepat
    if (now - _lastReconnectAttempt < _reconnectDelay)
        return false;

    _lastReconnectAttempt = now;

    // Cek WiFi dulu sebelum coba MQTT
    if (!isWifiConnected())
    {
        LOG_WARN(TAG, "WiFi terputus — mencoba reconnect WiFi...");
        if (!_connectWifi())
        {
            // Backoff berlaku juga untuk WiFi reconnect
            _reconnectDelay = min(_reconnectDelay * 2, RECONNECT_MAX_MS);
            LOG_WARN(TAG, "WiFi gagal — next retry dalam %lu ms", _reconnectDelay);
            return false;
        }
    }

    LOG_INFO(TAG, "Mencoba reconnect MQTT (delay was %lu ms)...", _reconnectDelay);

    if (_connectMqtt())
    {
        // Berhasil — reset backoff ke nilai awal
        _reconnectDelay = Mqtt::RECONNECT_DELAY_MS;
        return true;
    }

    // Gagal — naikkan backoff, maksimum RECONNECT_MAX_MS
    _reconnectDelay = min(_reconnectDelay * 2, RECONNECT_MAX_MS);
    LOG_WARN(TAG, "Reconnect gagal — next retry dalam %lu ms", _reconnectDelay);
    return false;
}


// =============================================================================
// _connectWifi() — Koneksi WiFi dalam Mode WIFI_AP_STA
//
// AP tersembunyi diaktifkan bukan untuk diakses, melainkan untuk mengunci
// channel radio agar ESP-NOW tidak terganggu channel hopping WiFi STA.
// =============================================================================
bool NetworkMqtt::_connectWifi()
{
    // Mode AP_STA: STA untuk router, AP untuk kunci channel ESP-NOW
    WiFi.mode(WIFI_AP_STA);

    // AP tersembunyi di channel 1 — tidak ada yang perlu konek ke sini
    WiFi.softAP("gw_ch_lock", "12345678", 1 /*channel*/, 1 /*hidden SSID*/);
    delay(100);

    LOG_INFO(TAG, "Konek ke WiFi '%s'...", Wifi::SSID);
    WiFi.begin(Wifi::SSID, Wifi::PASSWORD);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start > Timing::WIFI_TIMEOUT_MS)
        {
            LOG_ERROR(TAG, "WiFi timeout setelah %lu ms", Timing::WIFI_TIMEOUT_MS);
            return false;
        }
        delay(500);
    }

    // Verifikasi channel aktual setelah konek
    uint8_t ch;
    wifi_second_chan_t sch;
    esp_wifi_get_channel(&ch, &sch);

    LOG_INFO(TAG, "WiFi terhubung | IP=%s | channel=%d",
             WiFi.localIP().toString().c_str(), ch);

    if (ch != 1)
    {
        LOG_WARN(TAG, "Channel aktual=%d, bukan 1. "
                 "Kompile ulang sensor dengan ESPNOW_CHANNEL=%d", ch, ch);
    }

    return true;
}


// =============================================================================
// _connectMqtt() — Koneksi ke MQTT Broker
// =============================================================================
bool NetworkMqtt::_connectMqtt()
{
    LOG_INFO(TAG, "Konek ke broker %s:%d ...", Mqtt::BROKER, Mqtt::PORT);

    // Gunakan credentials jika tersedia, anonymous jika tidak
    const bool ok = (strlen(Mqtt::USER) > 0)
        ? _mqttClient.connect(Mqtt::CLIENT_ID, Mqtt::USER, Mqtt::PASSWORD)
        : _mqttClient.connect(Mqtt::CLIENT_ID);

    if (ok)
    {
        // Reset timer reconnect saat berhasil connect
        // Mencegah tryReconnect() langsung jalan setelah begin()
        _lastReconnectAttempt = millis();
        _reconnectDelay       = Mqtt::RECONNECT_DELAY_MS;

        LOG_INFO(TAG, "MQTT terhubung sebagai '%s'", Mqtt::CLIENT_ID);
        _publishOnlineStatus();
    }
    else
    {
        LOG_ERROR(TAG, "MQTT gagal | state=%d (%s)",
                  _mqttClient.state(),
                  _mqttStateStr(_mqttClient.state()));
    }

    return ok;
}


// =============================================================================
// _publishOnlineStatus() — Announce Gateway Online
//
// Publish retained message ke topic status saat gateway terhubung.
// Broker akan otomatis publish "offline" (Last Will) saat koneksi putus.
// =============================================================================
void NetworkMqtt::_publishOnlineStatus()
{
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/gateway/status", Mqtt::TOPIC_BASE);
    _mqttClient.publish(topic, "online", true /*retain*/);
    LOG_DEBUG(TAG, "Status published → %s : online", topic);
}


// =============================================================================
// _mqttStateStr() — Terjemahkan PubSubClient State Code ke String
//
// Memudahkan debugging tanpa harus buka dokumentasi PubSubClient setiap saat.
// =============================================================================
const char* NetworkMqtt::_mqttStateStr(int state)
{
    switch (state)
    {
        case -4: return "TIMEOUT";
        case -3: return "LOST";
        case -2: return "FAILED";
        case -1: return "DISCONNECTED";
        case  0: return "CONNECTED";
        case  1: return "BAD_PROTOCOL";
        case  2: return "BAD_CLIENT_ID";
        case  3: return "UNAVAILABLE";
        case  4: return "BAD_CREDENTIALS";
        case  5: return "UNAUTHORIZED";
        default: return "UNKNOWN";
    }
}

// =============================================================================
// NTP & Time Helpers
// =============================================================================
void NetworkMqtt::initNTP()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    LOG_INFO(TAG, "NTP dikonfigurasi (pool.ntp.org)");
}

uint32_t NetworkMqtt::getEpochS() const
{
    time_t now;
    time(&now);
    return (uint32_t)now;
}

uint16_t NetworkMqtt::getEpochMsPart() const
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_usec / 1000;
}