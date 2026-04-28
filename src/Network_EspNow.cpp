// =============================================================================
// Network_EspNow.cpp
//
// SENSOR (senderMode=true):
//   WiFi.mode(STA) → set channel=1 → esp_now_init → add peer gateway
//
// GATEWAY (senderMode=false):
//   WiFi sudah konek (dilakukan NetworkMqtt::begin() sebelum fungsi ini).
//   TIDAK WiFi.mode() / disconnect — langsung esp_now_init saja.
//   Channel sudah benar karena WiFi sudah konek ke router di channel 1.
// =============================================================================

#include "Network_EspNow.h"
#include <cstring>
#include <esp_wifi.h>

QueueHandle_t g_mqttQueue = nullptr;
static NetworkEspNow* _instance = nullptr;

static constexpr uint8_t ESPNOW_CHANNEL = 1; // channel terbukti ACK

// ---------------------------------------------------------------------------
bool NetworkEspNow::begin(bool senderMode) {
    _instance   = this;
    _senderMode = senderMode;

    if (senderMode) {
        // ===== SENSOR NODE =====
        // WiFi belum pernah di-init → lakukan dari awal
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        // Kunci channel agar cocok dengan gateway
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        // Verifikasi
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Channel sensor: %d %s\n",
                      ch, ch == ESPNOW_CHANNEL ? "✓" : "✗ MISMATCH!");

    } else {
        // ===== GATEWAY NODE =====
        // WiFi SUDAH konek lewat NetworkMqtt::begin() → JANGAN disconnect!
        // Channel sudah mengikuti router secara otomatis.
        // Cukup cetak channel aktual untuk konfirmasi.
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Gateway channel (ikut router): %d\n", ch);

        if (ch != ESPNOW_CHANNEL) {
            Serial.printf("[ESP-NOW] ⚠ PERINGATAN: channel gateway=%d tapi sensor dikunci ke %d!\n",
                          ch, ESPNOW_CHANNEL);
            Serial.printf("[ESP-NOW]   Ganti ESPNOW_CHANNEL=%d di Network_EspNow.cpp\n", ch);
            Serial.printf("[ESP-NOW]   lalu compile ulang sensor node.\n");
        }
    }

    // esp_now_init() — di gateway dipanggil SETELAH WiFi konek
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] ERROR: init gagal!");
        return false;
    }

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    if (senderMode) {
        if (!addPeer(MacAddr::GATEWAY)) return false;
        Serial.println("[ESP-NOW] Mode: SENDER → Gateway terdaftar");
    } else {
        if (!addPeer(MacAddr::NODE_A)) return false;
        if (!addPeer(MacAddr::NODE_B)) return false;
        Serial.println("[ESP-NOW] Mode: RECEIVER → Node A & B terdaftar");
    }

    Serial.printf("[ESP-NOW] MAC lokal: %s\n", WiFi.macAddress().c_str());
    return true;
}

// ---------------------------------------------------------------------------
bool NetworkEspNow::addPeer(const uint8_t* mac) {
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.printf("[ESP-NOW] ERROR: gagal tambah peer %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool NetworkEspNow::sendImu(const ImuPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(ImuPacket));
    return r == ESP_OK;
}

bool NetworkEspNow::sendPpg(const PpgPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(PpgPacket));
    return r == ESP_OK;
}

bool NetworkEspNow::sendHeartbeat(const HeartbeatPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(HeartbeatPacket));
    return r == ESP_OK;
}

// ---------------------------------------------------------------------------
void NetworkEspNow::onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (_instance) _instance->_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESP-NOW] WARN: pengiriman gagal (NACK)");
    }
}

// ---------------------------------------------------------------------------
void NetworkEspNow::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1 || !g_mqttQueue) return;

    PacketType type = static_cast<PacketType>(data[0]);
    MqttMessage msg{};
    uint8_t nodeId = data[1];

    switch (type) {
        case PacketType::IMU_DATA: {
            if (len < static_cast<int>(sizeof(ImuPacket))) break;
            const auto* pkt = reinterpret_cast<const ImuPacket*>(data);
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/imu", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                     "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f}",
                     pkt->header.timestamp,
                     pkt->data.accel_x, pkt->data.accel_y, pkt->data.accel_z,
                     pkt->data.gyro_x,  pkt->data.gyro_y,  pkt->data.gyro_z);
            break;
        }
        case PacketType::PPG_DATA: {
            if (len < static_cast<int>(sizeof(PpgPacket))) break;
            const auto* pkt = reinterpret_cast<const PpgPacket*>(data);
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/ppg", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"ir\":%lu,\"hr\":%d,\"valid\":%s}",
                     pkt->header.timestamp,
                     pkt->data.ir_raw,
                     pkt->data.heart_rate,
                     pkt->data.valid ? "true" : "false");
            break;
        }
        case PacketType::HEARTBEAT: {
            if (len < static_cast<int>(sizeof(HeartbeatPacket))) break;
            const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(data);
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"uptime\":%lu}",
                     pkt->header.timestamp, pkt->uptime_s);
            break;
        }
        default:
            Serial.printf("[ESP-NOW] WARN: tipe paket tidak dikenal: 0x%02X\n",
                          static_cast<uint8_t>(type));
            return;
    }

    if (xQueueSendFromISR(g_mqttQueue, &msg, nullptr) != pdTRUE) {
        Serial.println("[ESP-NOW] WARN: MQTT queue penuh, paket dibuang!");
    }
}