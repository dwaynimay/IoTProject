// =============================================================================
// Network_EspNow.cpp — Implementasi ESP-NOW
// =============================================================================

#include "Network_EspNow.h"
#include <cstring>

// Queue yang dibuat di main.cpp (gateway) atau tidak dipakai (sensor node)
QueueHandle_t g_mqttQueue = nullptr;

// Pointer ke instance aktif (untuk callback statis → akses member)
static NetworkEspNow* _instance = nullptr;

// ---------------------------------------------------------------------------
bool NetworkEspNow::begin(bool senderMode) {
    _instance   = this;
    _senderMode = senderMode;

    // ESP-NOW bekerja di WiFi Station mode, tapi tanpa koneksi AP
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] ERROR: init gagal!");
        return false;
    }

    // Daftarkan callback
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    if (senderMode) {
        // Node sensor → tambahkan gateway sebagai peer
        if (!addPeer(MacAddr::GATEWAY)) return false;
        Serial.println("[ESP-NOW] Mode: SENDER → Gateway terdaftar");
    } else {
        // Gateway → tambahkan semua sensor sebagai peer
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
    peer.channel = 0;       // auto-channel
    peer.encrypt = false;   // enkripsi dinonaktifkan (aktifkan jika perlu)

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
// Callback: dipanggil setelah send selesai (ACK/NACK)
void NetworkEspNow::onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (_instance) {
        _instance->_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
    }
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESP-NOW] WARN: pengiriman gagal (NACK)");
    }
}

// ---------------------------------------------------------------------------
// Callback: dipanggil saat data diterima (berjalan di WiFi task, BUKAN FreeRTOS task)
// JANGAN lakukan operasi berat di sini — gunakan queue.
void NetworkEspNow::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1 || !g_mqttQueue) return;

    PacketType type = static_cast<PacketType>(data[0]);

    // Bangun pesan MQTT berdasarkan tipe paket
    MqttMessage msg{};
    uint8_t nodeId = data[1]; // byte kedua selalu node_id

    switch (type) {
        case PacketType::IMU_DATA: {
            if (len < static_cast<int>(sizeof(ImuPacket))) break;
            const auto* pkt = reinterpret_cast<const ImuPacket*>(data);
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/imu", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                     "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"temp\":%.2f}",
                     pkt->header.timestamp,
                     pkt->data.accel_x, pkt->data.accel_y, pkt->data.accel_z,
                     pkt->data.gyro_x,  pkt->data.gyro_y,  pkt->data.gyro_z,
                     pkt->data.temp_c);
            break;
        }
        case PacketType::PPG_DATA: {
            if (len < static_cast<int>(sizeof(PpgPacket))) break;
            const auto* pkt = reinterpret_cast<const PpgPacket*>(data);
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/ppg", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"red\":%lu,\"ir\":%lu,"
                     "\"spo2\":%.1f,\"hr\":%d,\"valid\":%s}",
                     pkt->header.timestamp,
                     pkt->data.red_raw, pkt->data.ir_raw,
                     pkt->data.spo2, pkt->data.heart_rate,
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

    // Kirim ke queue (non-blocking — jika penuh, data dibuang)
    if (xQueueSendFromISR(g_mqttQueue, &msg, nullptr) != pdTRUE) {
        Serial.println("[ESP-NOW] WARN: MQTT queue penuh, paket dibuang!");
    }
}