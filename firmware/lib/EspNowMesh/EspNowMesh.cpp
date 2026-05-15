// File: firmware/lib/EspNowMesh/EspNowMesh.cpp

// =============================================================================
// EspNowMesh.cpp — Implementasi Transport Layer ESP-NOW
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
//
// ⚠️  PERATURAN ISR (_onDataRecv):
//   TIDAK BOLEH ada di dalam _onDataRecv:
//     ✗ LOG_* / Serial.printf
//     ✗ malloc / new / delete
//     ✗ logika bisnis apapun
//   BOLEH:
//     ✓ memcpy (< 250 bytes, ~1µs)
//     ✓ xQueueSendFromISR
//     ✓ perbandingan integer sederhana
// =============================================================================

#include "EspNowMesh.h"

static constexpr char TAG[]           = "MESH";
static constexpr uint8_t ESPNOW_CHANNEL = 1;

// Definisi storage untuk variable extern dan static member
QueueHandle_t g_rawQueue  = nullptr;
QueueHandle_t g_mqttQueue = nullptr;
EspNowMesh*   EspNowMesh::_instance = nullptr;


// =============================================================================
// begin() — Inisialisasi ESP-NOW
// =============================================================================
bool EspNowMesh::begin(bool senderMode)
{
    _instance   = this;
    _senderMode = senderMode;

    if (senderMode)
    {
        // ── Sensor Node ───────────────────────────────────────────────────────
        // Set channel manual sebelum esp_now_init agar match dengan gateway
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        // Verifikasi channel berhasil di-set
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);

        if (ch != ESPNOW_CHANNEL)
        {
            LOG_WARN(TAG, "Channel mismatch: set=%d actual=%d",
                     ESPNOW_CHANNEL, ch);
        }
        else
        {
            LOG_DEBUG(TAG, "Channel sensor dikunci ke %d", ch);
        }
    }
    else
    {
        // ── Gateway Node ──────────────────────────────────────────────────────
        // Channel gateway mengikuti router — verifikasi saja
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);

        LOG_INFO(TAG, "Gateway channel (ikut router): %d", ch);

        if (ch != ESPNOW_CHANNEL)
        {
            LOG_WARN(TAG, "Channel gateway=%d, sensor dikunci ke %d. "
                     "Kompile ulang sensor dengan channel=%d",
                     ch, ESPNOW_CHANNEL, ch);
        }
    }

    // Init ESP-NOW
    if (esp_now_init() != ESP_OK)
    {
        LOG_ERROR(TAG, "esp_now_init() gagal!");
        return false;
    }

    esp_now_register_send_cb(_onDataSent);
    esp_now_register_recv_cb(_onDataRecv);

    // Daftarkan peer sesuai role
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
        LOG_INFO(TAG, "ISR pipeline: onDataRecv → g_rawQueue → taskMeshHandler");
    }

    return true;
}


// =============================================================================
// sendCsAxis() — Kirim Satu Axis CS
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

    // Jeda antar paket — mencegah buffer overflow di gateway
    vTaskDelay(pdMS_TO_TICKS(1));

    return ok;
}


// =============================================================================
// sendCsPpg() — Kirim Data PPG CS
// =============================================================================
bool EspNowMesh::sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                            int8_t heartRate, bool ppgValid,
                            bool fingerOn, uint32_t timestamp)
{
    CSPpgPacket pkt{};
    pkt.header    = { PacketType::CS_IR, nodeId, timestamp };
    memcpy(pkt.yIr, yIr, CS_M * sizeof(float));
    pkt.heartRate = heartRate;
    pkt.ppgValid  = ppgValid;
    pkt.edge      = { fingerOn, 0 };

    return _send(&pkt, sizeof(CSPpgPacket));
}


// =============================================================================
// sendCombined() — Kirim CombinedPacket
// =============================================================================
bool EspNowMesh::sendCombined(const CombinedPacket& pkt)
{
    return _send(&pkt, sizeof(CombinedPacket));
}


// =============================================================================
// sendHeartbeat() — Kirim Heartbeat Periodik
// =============================================================================
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
// _send() — Helper Kirim ke Gateway
// =============================================================================
bool EspNowMesh::_send(const void* data, size_t len)
{
    const esp_err_t err = esp_now_send(
        MacAddr::GATEWAY,
        reinterpret_cast<const uint8_t*>(data),
        len
    );

    if (err != ESP_OK)
    {
        LOG_EVERY_N(10, LOG_WARN, TAG, "esp_now_send gagal: 0x%X", err);
    }

    return err == ESP_OK;
}


// =============================================================================
// _addPeer() — Daftarkan Satu Peer ke ESP-NOW
// =============================================================================
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
// _onDataSent() — Callback Pengiriman
// =============================================================================
void EspNowMesh::_onDataSent(const uint8_t* mac, esp_now_send_status_t status)
{
    if (_instance)
        _instance->_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);

    // LOG tidak aman di sini (WiFi task context) — hanya update flag
    // Task sender akan baca _lastSendOk via lastSendOk()
}


// =============================================================================
// _onDataRecv() — ISR Penerima (HANYA memcpy!)
//
// ⚠️  JANGAN TAMBAH KODE APAPUN DI SINI kecuali ada alasan sangat kuat.
//     Semua logika routing ada di MeshRouting.cpp yang dipanggil dari
//     taskMeshHandler di task context (aman untuk LOG_*, snprintf, dsb).
// =============================================================================
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