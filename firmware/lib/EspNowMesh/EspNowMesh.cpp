// File: firmware/lib/EspNowMesh/EspNowMesh.cpp

// =============================================================================
// EspNowMesh.cpp — Implementasi Transport Layer ESP-NOW
// =============================================================================
//
// PERUBAHAN v2 (refactor):
//   - g_mqttQueue DIHAPUS dari file ini. g_mqttQueue adalah milik
//     task_mesh_handler.cpp (layer di atasnya). Modul transport tidak boleh
//     tahu tentang MQTT queue.
//   - g_rawQueue tetap di sini karena ISR _onDataRecv push ke sini.
//   - Deklarasi extern g_mqttQueue dihapus dari EspNowMesh.h juga.
//
// KEPEMILIKAN QUEUE:
//   g_rawQueue  → didefinisikan di EspNowMesh.cpp (transport layer)
//   g_mqttQueue → didefinisikan di task_mesh_handler.cpp (app layer)
//   Keduanya di-extern di file yang membutuhkan.
//
// ⚠️  PERATURAN ISR (_onDataRecv):
//   TIDAK BOLEH ada: LOG_*, Serial, malloc, logika bisnis
//   BOLEH: memcpy (<250 bytes), xQueueSendFromISR, perbandingan integer
// =============================================================================

#include <esp_wifi.h>
#include "EspNowMesh.h"

static constexpr char TAG[]             = "MESH";
static constexpr uint8_t ESPNOW_CHANNEL = 1;

QueueHandle_t g_rawQueue  = nullptr;
extern QueueHandle_t g_mqttQueue;

EspNowMesh* EspNowMesh::_instance = nullptr;


// =============================================================================
// begin()
// =============================================================================
bool EspNowMesh::begin(bool senderMode)
{
    _instance   = this;
    _senderMode = senderMode;

    if (senderMode)
    {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);

        if (ch != ESPNOW_CHANNEL)
            LOG_WARN(TAG, "Channel mismatch: set=%d actual=%d", ESPNOW_CHANNEL, ch);
        else
            LOG_DEBUG(TAG, "Channel sensor dikunci ke %d", ch);
    }
    else
    {
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        LOG_INFO(TAG, "Gateway channel (ikut router): %d", ch);

        if (ch != ESPNOW_CHANNEL)
            LOG_WARN(TAG, "Channel gateway=%d, sensor dikunci ke %d. "
                     "Kompile ulang sensor dengan channel=%d",
                     ch, ESPNOW_CHANNEL, ch);
    }

    if (esp_now_init() != ESP_OK)
    {
        LOG_ERROR(TAG, "esp_now_init() gagal!");
        return false;
    }

    esp_now_register_send_cb(_onDataSent);
    esp_now_register_recv_cb(_onDataRecv);

    if (senderMode)
    {
        if (!_addPeer(MacAddr::GATEWAY)) return false;
        LOG_INFO(TAG, "Mode: SENDER | Gateway terdaftar | MAC lokal: %s",
                 WiFi.macAddress().c_str());
    }
    else
    {
        if (!_addPeer(MacAddr::NODE_A)) return false;
        if (!_addPeer(MacAddr::NODE_B)) return false;
        LOG_INFO(TAG, "Mode: RECEIVER | Node A & B terdaftar | MAC lokal: %s",
                 WiFi.macAddress().c_str());
    }

    return true;
}


// =============================================================================
// Send API
// =============================================================================
bool EspNowMesh::sendCsAxis(uint8_t pktType, uint8_t nodeId,
                             const float y[CS_M], bool fingerOn,
                             uint32_t timestamp)
{
    CS1AxisPacket pkt{};
    pkt.header = { static_cast<PacketType>(pktType), nodeId, timestamp };
    memcpy(pkt.y, y, CS_M * sizeof(float));
    pkt.edge   = { fingerOn, 0 };

    const bool ok = _send(&pkt, sizeof(CS1AxisPacket));
    vTaskDelay(pdMS_TO_TICKS(1));
    return ok;
}

bool EspNowMesh::sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                            int8_t heartRate, bool ppgValid,
                            float spo2,
                            bool fingerOn, uint32_t timestamp)
{
    CSPpgPacket pkt{};
    pkt.header    = { PacketType::CS_IR, nodeId, timestamp };
    memcpy(pkt.yIr, yIr, CS_M * sizeof(float));
    pkt.heartRate = heartRate;
    pkt.ppgValid  = ppgValid;
    pkt.spo2      = spo2;      // ← SpO2 dari SensorPPG
    pkt.edge      = { fingerOn, 0 };

    return _send(&pkt, sizeof(CSPpgPacket));
}

bool EspNowMesh::sendCombined(const CombinedPacket& pkt)
{
    return _send(&pkt, sizeof(CombinedPacket));
}

bool EspNowMesh::sendHeartbeat(uint8_t nodeId, uint32_t uptimeS)
{
    HeartbeatPacket pkt{};
    pkt.header  = { PacketType::HEARTBEAT, nodeId,
                    static_cast<uint32_t>(millis()) };
    pkt.uptimeS = uptimeS;
    pkt.rssi    = 0;
    return _send(&pkt, sizeof(HeartbeatPacket));
}


// =============================================================================
// Private Helpers
// =============================================================================
bool EspNowMesh::_send(const void* data, size_t len)
{
    const esp_err_t err = esp_now_send(
        MacAddr::GATEWAY,
        reinterpret_cast<const uint8_t*>(data),
        len
    );

    if (err != ESP_OK)
        LOG_EVERY_N(10, LOG_WARN, TAG, "esp_now_send gagal: 0x%X", err);

    return err == ESP_OK;
}

bool EspNowMesh::_addPeer(const uint8_t* mac)
{
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK)
    {
        LOG_ERROR(TAG, "Gagal daftarkan peer %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }

    LOG_DEBUG(TAG, "Peer terdaftar: %02X:%02X:%02X:%02X:%02X:%02X",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}


// =============================================================================
// Callbacks
// =============================================================================
static volatile uint32_t _ackCount  = 0;
static volatile uint32_t _nackCount = 0;

void EspNowMesh::_onDataSent(const uint8_t* mac, esp_now_send_status_t status)
{
    if (_instance)
        _instance->_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);

    if (status == ESP_NOW_SEND_SUCCESS)
    {
        _ackCount++;
        LOG_EVERY_N(50, LOG_DEBUG, "MESH", "ESP-NOW ACK (total=%lu)", _ackCount);
    }
    else
    {
        _nackCount++;
        LOG_EVERY_N(5, LOG_WARN, "MESH",
                    "ESP-NOW NACK (total=%lu, rate=%.1f%%)",
                    _nackCount,
                    100.0f * _nackCount / (_ackCount + _nackCount));
    }
}

// ISR — HANYA memcpy
void EspNowMesh::_onDataRecv(const uint8_t* mac, const uint8_t* data, int len)
{
    if (len < 1 || !g_rawQueue) return;

    RawPacket raw{};
    raw.len = static_cast<uint8_t>(len <= 250 ? len : 250);
    memcpy(raw.data,   data, raw.len);
    memcpy(raw.srcMac, mac,  6);

    BaseType_t higherPriorityWoken = pdFALSE;
    xQueueSendFromISR(g_rawQueue, &raw, &higherPriorityWoken);

    if (higherPriorityWoken == pdTRUE)
        portYIELD_FROM_ISR();
}