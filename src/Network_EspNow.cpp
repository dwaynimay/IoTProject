// =============================================================================
// Network_EspNow.cpp
//
// SENSOR (senderMode=true):
//   WiFi.mode(STA) → set channel=1 → esp_now_init → add peer gateway
//   Kirim CombinedPacket setiap SEND_INTERVAL_MS (throttled di main.cpp)
//
// GATEWAY (senderMode=false):
//   WiFi sudah konek (dilakukan NetworkMqtt::begin() sebelum fungsi ini).
//   onDataRecv menerima CombinedPacket → format JSON → push ke g_mqttQueue
//   Batching dikontrol via BatchConfig::BATCHING_ENABLED di Config.h
// =============================================================================

#include "Network_EspNow.h"
#include <cstring>
#include <esp_wifi.h>

QueueHandle_t g_mqttQueue = nullptr;
static NetworkEspNow* _instance = nullptr;

static constexpr uint8_t ESPNOW_CHANNEL = 1;

// ---------------------------------------------------------------------------
// Batching state — per node_id (support node 1 dan 2)
// Aktif hanya jika BatchConfig::BATCHING_ENABLED == true
// ---------------------------------------------------------------------------
#if NODE_ROLE == ROLE_GATEWAY

struct BatchBuffer {
    // Buffer menyimpan JSON entry per sampel, digabung saat penuh
    // Format entry: {"ts":...,"ax":...,...,"ir":...,"hr":...,"finger":...}
    static constexpr uint8_t MAX_BATCH = 10; // batas atas hardcoded
    char     entries[MAX_BATCH][180];         // setiap entry max 180 char
    uint8_t  count    = 0;
    uint8_t  node_id  = 0;
};

// Buffer untuk node 1 dan node 2 (index 0 = node 1, index 1 = node 2)
static BatchBuffer g_batchBuf[2];

// Ambil index buffer berdasarkan node_id (1→0, 2→1, lainnya→0)
static inline uint8_t batchIdx(uint8_t node_id) {
    return (node_id >= 1 && node_id <= 2) ? (node_id - 1) : 0;
}

#endif // ROLE_GATEWAY

// ---------------------------------------------------------------------------
bool NetworkEspNow::begin(bool senderMode) {
    _instance   = this;
    _senderMode = senderMode;

    if (senderMode) {
        // ===== SENSOR NODE =====
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Channel sensor: %d %s\n",
                      ch, ch == ESPNOW_CHANNEL ? "OK" : "MISMATCH!");
    } else {
        // ===== GATEWAY NODE =====
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Gateway channel (ikut router): %d\n", ch);

        if (ch != ESPNOW_CHANNEL) {
            Serial.printf("[ESP-NOW] WARN: channel gateway=%d, sensor dikunci ke %d!\n",
                          ch, ESPNOW_CHANNEL);
            Serial.printf("[ESP-NOW] Ubah ESPNOW_CHANNEL=%d lalu compile ulang sensor.\n", ch);
        }
    }

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
        Serial.printf("[ESP-NOW] Batching: %s (size=%d)\n",
                      BatchConfig::BATCHING_ENABLED ? "AKTIF" : "NONAKTIF",
                      BatchConfig::BATCH_SIZE);
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
bool NetworkEspNow::sendCombined(const CombinedPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(CombinedPacket));
    if (r != ESP_OK) {
        Serial.printf("[ESP-NOW] sendCombined GAGAL: esp_err=0x%X (%s)\n",
                      r,
                      r == ESP_ERR_ESPNOW_NOT_INIT  ? "NOT_INIT" :
                      r == ESP_ERR_ESPNOW_ARG        ? "ARG_INVALID" :
                      r == ESP_ERR_ESPNOW_INTERNAL   ? "INTERNAL" :
                      r == ESP_ERR_ESPNOW_NO_MEM     ? "NO_MEM" :
                      r == ESP_ERR_ESPNOW_NOT_FOUND  ? "PEER_NOT_FOUND" :
                      r == ESP_ERR_ESPNOW_IF         ? "INTERFACE_ERR" : "UNKNOWN");
    }
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
// onDataRecv — berjalan di ISR context, harus cepat.
// Semua format JSON dilakukan di sini lalu push ke queue.
// Untuk CombinedPacket: routing berdasarkan node_id → topic node_<id>/combined
// ---------------------------------------------------------------------------
void NetworkEspNow::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1 || !g_mqttQueue) return;

    PacketType type   = static_cast<PacketType>(data[0]);
    uint8_t    nodeId = data[1];

    // -----------------------------------------------------------------------
    // Handle CombinedPacket
    // -----------------------------------------------------------------------
    if (type == PacketType::COMBINED_DATA) {
        if (len < static_cast<int>(sizeof(CombinedPacket))) return;
        const auto* pkt = reinterpret_cast<const CombinedPacket*>(data);

        // Format entry JSON untuk sampel ini
        char entry[180];
        snprintf(entry, sizeof(entry),
                 "{"
                 "\"ts\":%lu,"
                 "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                 "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                 "\"ir\":%lu,\"red\":%lu,"
                 "\"hr\":%d,\"spo2\":%.1f,\"ppg_valid\":%s,"
                 "\"finger\":%s"
                 "}",
                 pkt->header.timestamp,
                 pkt->imu.accel_x, pkt->imu.accel_y, pkt->imu.accel_z,
                 pkt->imu.gyro_x,  pkt->imu.gyro_y,  pkt->imu.gyro_z,
                 (unsigned long)pkt->ppg.ir_raw, (unsigned long)pkt->ppg.red_raw,
                 pkt->ppg.heart_rate, pkt->ppg.spo2,
                 pkt->ppg.valid   ? "true" : "false",
                 pkt->edge.finger_on ? "true" : "false");

#if NODE_ROLE == ROLE_GATEWAY
        if (BatchConfig::BATCHING_ENABLED) {
            // ---------------------------------------------------------------
            // MODE BATCH: kumpulkan sampai BATCH_SIZE, baru publish 1 array
            // ---------------------------------------------------------------
            uint8_t idx = batchIdx(nodeId);
            BatchBuffer& buf = g_batchBuf[idx];
            buf.node_id = nodeId;

            uint8_t batchSize = (BatchConfig::BATCH_SIZE <= BatchBuffer::MAX_BATCH)
                                ? BatchConfig::BATCH_SIZE
                                : BatchBuffer::MAX_BATCH;

            if (buf.count < batchSize) {
                strncpy(buf.entries[buf.count], entry, sizeof(buf.entries[0]) - 1);
                buf.count++;
            }

            if (buf.count >= batchSize) {
                // Batch penuh → gabungkan jadi JSON array dan push ke queue
                MqttMessage msg{};
                snprintf(msg.topic, sizeof(msg.topic),
                         "%s/node_%d/combined", Mqtt::TOPIC_BASE, nodeId);

                // Gabungkan: [entry0,entry1,...,entryN]
                int pos = 0;
                msg.payload[pos++] = '[';
                for (uint8_t i = 0; i < buf.count && pos < (int)sizeof(msg.payload) - 2; i++) {
                    if (i > 0) { msg.payload[pos++] = ','; }
                    int remaining = sizeof(msg.payload) - pos - 2;
                    int written   = snprintf(msg.payload + pos, remaining, "%s", buf.entries[i]);
                    if (written > 0 && written < remaining) pos += written;
                }
                msg.payload[pos++] = ']';
                msg.payload[pos]   = '\0';

                xQueueSendFromISR(g_mqttQueue, &msg, nullptr);
                buf.count = 0; // reset buffer
            }
        } else {
#endif
            // ---------------------------------------------------------------
            // MODE LANGSUNG: setiap sampel langsung publish (1 objek JSON)
            // ---------------------------------------------------------------
            MqttMessage msg{};
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/combined", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload), "%s", entry);

            if (xQueueSendFromISR(g_mqttQueue, &msg, nullptr) != pdTRUE) {
                Serial.println("[ESP-NOW] WARN: queue penuh, paket dibuang!");
            }
#if NODE_ROLE == ROLE_GATEWAY
        }
#endif
        return;
    }

    // -----------------------------------------------------------------------
    // Handle Heartbeat (tetap dipertahankan)
    // -----------------------------------------------------------------------
    if (type == PacketType::HEARTBEAT) {
        if (len < static_cast<int>(sizeof(HeartbeatPacket))) return;
        const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(data);
        MqttMessage msg{};
        snprintf(msg.topic, sizeof(msg.topic),
                 "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, nodeId);
        snprintf(msg.payload, sizeof(msg.payload),
                 "{\"ts\":%lu,\"uptime\":%lu}",
                 pkt->header.timestamp, (unsigned long)pkt->uptime_s);
        xQueueSendFromISR(g_mqttQueue, &msg, nullptr);
        return;
    }

    // Tipe lama (IMU_DATA / PPG_DATA terpisah) — log peringatan
    Serial.printf("[ESP-NOW] WARN: tipe paket lama/tidak dikenal: 0x%02X — pakai CombinedPacket\n",
                  static_cast<uint8_t>(type));
}