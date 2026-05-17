// File: firmware/lib/EspNowMesh/EspNowMesh.cpp

// =============================================================================
// EspNowMesh.cpp — Implementasi Transport Layer ESP-NOW Multi-Hop
// =============================================================================
//
// PERUBAHAN v3.0:
//   - sendBeacon()      : gateway broadcast beacon
//   - sendRssiReport()  : node kirim RSSI ke neighbor
//   - forwardRoutedCs() : relay node bungkus + forward ke gateway
//   - _promiscuousRxCb(): baca RSSI dari beacon via promiscuous mode
//   - _onDataRecv()     : handle RoutedCsPacket (unwrap lalu push ke rawQueue)
//   - _send()           : sekarang terima dstMac sebagai parameter
//
// TEKNIK BACA RSSI:
//   ESP-NOW recv callback (esp_now_recv_cb_t) tidak memberikan info RSSI.
//   Workaround: aktifkan wifi promiscuous mode, filter frame yang cocok
//   dengan MAC gateway (beacon source), baca wifi_pkt_rx_ctrl_t.rssi.
//   Promiscuous mode diaktifkan HANYA di sensor node, tidak di gateway.
//
// ⚠️  ATURAN ISR (_onDataRecv, _promiscuousRxCb):
//   TIDAK BOLEH: LOG_*, Serial, malloc, logika bisnis berat
//   BOLEH: memcpy, perbandingan, xQueueSendFromISR, update volatile var
// =============================================================================

#include <esp_wifi.h>
#include "EspNowMesh.h"

static constexpr char TAG[]             = "MESH";
static uint8_t s_channel = 1; // diisi dinamis saat begin()

// Broadcast MAC untuk beacon (semua node terima)
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
        // ── Sensor node: kunci channel, aktifkan promiscuous untuk RSSI ──────
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        // Reset sentinel — _promiscuousRxCb akan mengisi saat beacon terdeteksi
        s_channel = 0;

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(_promiscuousRxCb);

        // Channel sweep: coba ch 1-13 sampai beacon gateway terdeteksi.
        // Gateway broadcast beacon setiap 1000 ms; dwell 400 ms/ch.
        // Worst-case: 13 × 400 ms = 5.2 s (masih di dalam discovery phase 6 s).
        LOG_INFO(TAG, "Channel sweep dimulai (ch 1-13, dwell=400 ms/ch)...");

        for (uint8_t tryC = 1; tryC <= 13 && s_channel == 0; tryC++)
        {
            esp_wifi_set_channel(tryC, WIFI_SECOND_CHAN_NONE);
            delay(400); // _promiscuousRxCb isi s_channel jika beacon terdeteksi
        }

        if (s_channel == 0)
        {
            // Gateway tidak ditemukan — fallback, sync terjadi saat beacon pertama
            s_channel = 6;
            esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
            LOG_WARN(TAG, "Sweep selesai, beacon tidak ditemukan — fallback ch=%d", s_channel);
        }

        LOG_INFO(TAG, "Sensor channel: %d | promiscuous=ON (RSSI mode)", s_channel);
    }
    else
    {
        // ── Gateway node: tidak butuh promiscuous ─────────────────────────────
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        s_channel = (ch > 0 && ch <= 13) ? ch : 6;

        LOG_INFO(TAG, "Gateway channel: %d (dinamis, ikut router)", s_channel);
        // Tidak ada warning mismatch lagi
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
        // Sensor node: register gateway + neighbor sebagai peer
        if (!_addPeer(MacAddr::GATEWAY)) return false;

        // Register neighbor berdasarkan NODE_ID
        // NODE_ID 1 → neighbor = NODE_B, NODE_ID 2 → neighbor = NODE_A
        #if NODE_ID == 1
            if (!_addPeer(MacAddr::NODE_B)) return false;
            LOG_INFO(TAG, "Sensor node %d | peers: GATEWAY + NODE_B", NODE_ID);
        #else
            if (!_addPeer(MacAddr::NODE_A)) return false;
            LOG_INFO(TAG, "Sensor node %d | peers: GATEWAY + NODE_A", NODE_ID);
        #endif

        // Register broadcast MAC untuk menerima beacon
        if (!_addPeer(BROADCAST_MAC)) return false;

        LOG_INFO(TAG, "Mode: SENSOR | MAC lokal: %s", WiFi.macAddress().c_str());
    }
    else
    {
        // Gateway node: register semua sensor node sebagai peer
        if (!_addPeer(MacAddr::NODE_A)) return false;
        if (!_addPeer(MacAddr::NODE_B)) return false;

        // Broadcast MAC untuk sendBeacon() — WAJIB didaftarkan
        if (!_addPeer(BROADCAST_MAC)) return false;

        LOG_INFO(TAG, "Mode: GATEWAY | Node A & B + broadcast terdaftar | MAC: %s",
                 WiFi.macAddress().c_str());
    }

    return true;
}


// =============================================================================
// sendBeacon() — Gateway broadcast beacon untuk RSSI discovery
// =============================================================================
bool EspNowMesh::sendBeacon()
{
    BeaconPacket pkt{};
    pkt.header = {
        PacketType::BEACON,
        RoutingCfg::GATEWAY_NODE_ID,
        static_cast<uint32_t>(millis())
    };
    pkt.seqNum = _beaconSeqNum++;

    const bool ok = _send(&pkt, sizeof(BeaconPacket), BROADCAST_MAC);

    LOG_DEBUG(TAG, "Beacon #%d broadcast | ts=%lu",
              pkt.seqNum, (unsigned long)pkt.header.timestamp);
    return ok;
}


// =============================================================================
// sendRssiReport() — Sensor kirim RSSI-nya ke neighbor
// =============================================================================
bool EspNowMesh::sendRssiReport(uint8_t selfNodeId, int8_t rssiToGateway)
{
    RssiReportPacket pkt{};
    pkt.header = {
        PacketType::RSSI_REPORT,
        selfNodeId,
        static_cast<uint32_t>(millis())
    };
    pkt.rssiToGateway = rssiToGateway;
    pkt.hopCount      = 1;  // node ini 1 hop dari gateway
    pkt.reserved      = 0;

    // Kirim ke neighbor (bukan ke gateway)
    #if NODE_ID == 1
        const uint8_t* neighborMac = MacAddr::NODE_B;
    #else
        const uint8_t* neighborMac = MacAddr::NODE_A;
    #endif

    const bool ok = _send(&pkt, sizeof(RssiReportPacket), neighborMac);

    LOG_DEBUG(TAG, "RssiReport → neighbor | self rssi=%d dBm | ok=%s",
              rssiToGateway, ok ? "Y" : "N");
    return ok;
}


// =============================================================================
// sendCsAxis() — Kirim satu axis CS ke tujuan (gateway atau neighbor)
// =============================================================================
bool EspNowMesh::sendCsAxis(uint8_t pktType, uint8_t nodeId,
                             const float y[CS_M], bool fingerOn,
                             uint32_t timestamp,
                             const uint8_t* dstMac)
{
    CS1AxisPacket pkt{};
    pkt.header = { static_cast<PacketType>(pktType), nodeId, timestamp };
    memcpy(pkt.y, y, CS_M * sizeof(float));
    pkt.edge   = { fingerOn, 0 };

    const bool ok = _send(&pkt, sizeof(CS1AxisPacket), dstMac);
    vTaskDelay(pdMS_TO_TICKS(1));
    return ok;
}


// =============================================================================
// sendCsPpg() — Kirim CS PPG ke tujuan (gateway atau neighbor)
// =============================================================================
bool EspNowMesh::sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                            int8_t heartRate, bool ppgValid,
                            float spo2, bool fingerOn,
                            uint32_t timestamp,
                            const uint8_t* dstMac)
{
    CSPpgPacket pkt{};
    pkt.header    = { PacketType::CS_IR, nodeId, timestamp };
    memcpy(pkt.yIr, yIr, CS_M * sizeof(float));
    pkt.heartRate = heartRate;
    pkt.ppgValid  = ppgValid;
    pkt.spo2      = spo2;
    pkt.edge      = { fingerOn, 0 };

    return _send(&pkt, sizeof(CSPpgPacket), dstMac);
}


// =============================================================================
// forwardRoutedCs() — Relay node bungkus paket CS dan forward ke gateway
// =============================================================================
bool EspNowMesh::forwardRoutedCs(uint8_t relayNodeId,
                                  uint8_t originalNodeId,
                                  const uint8_t* innerData,
                                  uint8_t innerLen)
{
    if (innerLen > sizeof(RoutedCsPacket::inner))
    {
        LOG_ERROR(TAG, "forwardRoutedCs: innerLen=%d terlalu besar (max %d)",
                  innerLen, (int)sizeof(RoutedCsPacket::inner));
        return false;
    }

    RoutedCsPacket pkt{};
    pkt.header.type           = PacketType::ROUTED_CS;
    pkt.header.relayNodeId    = relayNodeId;
    pkt.header.originalNodeId = originalNodeId;
    pkt.header.innerLen       = innerLen;
    pkt.header.relayTimestamp = static_cast<uint32_t>(millis());
    memcpy(pkt.inner, innerData, innerLen);

    const uint8_t totalLen = sizeof(RoutedCsHeader) + innerLen;
    const bool ok = _send(&pkt, totalLen, MacAddr::GATEWAY);

    LOG_DEBUG(TAG, "Forward ROUTED_CS | original_node=%d relay=%d innerLen=%d | ok=%s",
              originalNodeId, relayNodeId, innerLen, ok ? "Y" : "N");
    return ok;
}


// =============================================================================
// sendCombined() + sendHeartbeat() — tidak berubah dari v2
// =============================================================================
bool EspNowMesh::sendCombined(const CombinedPacket& pkt)
{
    return _send(&pkt, sizeof(CombinedPacket), MacAddr::GATEWAY);
}

bool EspNowMesh::sendHeartbeat(uint8_t nodeId, uint32_t uptimeS)
{
    HeartbeatPacket pkt{};
    pkt.header  = { PacketType::HEARTBEAT, nodeId,
                    static_cast<uint32_t>(millis()) };
    pkt.uptimeS = uptimeS;
    pkt.rssi    = static_cast<uint8_t>(-_lastBeaconRssi); // abs dBm
    return _send(&pkt, sizeof(HeartbeatPacket), MacAddr::GATEWAY);
}


// =============================================================================
// Private Helpers
// =============================================================================
bool EspNowMesh::_send(const void* data, size_t len, const uint8_t* dstMac)
{
    const esp_err_t err = esp_now_send(
        dstMac,
        reinterpret_cast<const uint8_t*>(data),
        len
    );

    if (err != ESP_OK)
        LOG_EVERY_N(10, LOG_WARN, TAG, "esp_now_send gagal: 0x%X", err);

    _lastSendOk = (err == ESP_OK);
    return _lastSendOk;
}

bool EspNowMesh::_addPeer(const uint8_t* mac)
{
    // Cek apakah sudah terdaftar
    if (_isPeerRegistered(mac)) return true;

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = s_channel;
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

bool EspNowMesh::_isPeerRegistered(const uint8_t* mac)
{
    esp_now_peer_info_t info{};
    return esp_now_get_peer(mac, &info) == ESP_OK;
}


// =============================================================================
// Callbacks
// =============================================================================
static volatile uint32_t _ackCount  = 0;
static volatile uint32_t _nackCount = 0;

void EspNowMesh::_onDataSent(const uint8_t* mac, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        _ackCount++;
        if (_instance) _instance->_lastSendOk = true;
    }
    else
    {
        _nackCount++;
        if (_instance) _instance->_lastSendOk = false;
        LOG_EVERY_N(5, LOG_WARN, "MESH",
                    "NACK (total=%lu, rate=%.1f%%)",
                    _nackCount,
                    100.0f * _nackCount / (_ackCount + _nackCount));
    }
}

// =============================================================================
// _onDataRecv() — ISR: terima paket, push ke rawQueue
//
// PERUBAHAN v3: handle ROUTED_CS dan RSSI_REPORT di sini.
//   - RSSI_REPORT: push ke rawQueue (diproses taskMeshHandler di sensor node)
//   - ROUTED_CS  : push ke rawQueue as-is (MeshRouting yang unwrap)
//   - Semua tipe lain: push ke rawQueue seperti sebelumnya
//
// ⚠️  Tetap MINIMAL — hanya memcpy + xQueueSendFromISR
// =============================================================================
void EspNowMesh::_onDataRecv(const uint8_t* mac, const uint8_t* data, int len)
{
    if (len < 1) return;

    const uint8_t pktType = data[0];

    // ── RSSI_REPORT: proses langsung di sensor node (tidak lewat queue) ──────
    // Di sensor node g_rawQueue == nullptr, tapi kita tetap perlu update router.
    if (pktType == static_cast<uint8_t>(PacketType::RSSI_REPORT))
    {
        if (len >= static_cast<int>(sizeof(RssiReportPacket)) && g_routerPtr)
        {
            const auto* pkt = reinterpret_cast<const RssiReportPacket*>(data);
            g_routerPtr->updateNeighborRssi(pkt->header.nodeId,
                                            pkt->rssiToGateway);
        }
        // Jangan push ke rawQueue — tidak perlu diproses lebih lanjut
        return;
    }

    // ── Semua tipe lain: push ke rawQueue (gateway) atau abaikan (sensor) ────
    if (!g_rawQueue) return;

    RawPacket raw{};
    raw.len = static_cast<uint8_t>(len <= 250 ? len : 250);
    memcpy(raw.data,   data, raw.len);
    memcpy(raw.srcMac, mac,  6);

    BaseType_t higherPriorityWoken = pdFALSE;
    xQueueSendFromISR(g_rawQueue, &raw, &higherPriorityWoken);

    if (higherPriorityWoken == pdTRUE)
        portYIELD_FROM_ISR();
}

// =============================================================================
// _promiscuousRxCb() — Baca RSSI dari beacon via promiscuous mode
//
// Hanya aktif di sensor node. Filter paket berdasarkan:
//   1. Tipe frame = data (bukan management/control)
//   2. Byte pertama payload = PacketType::BEACON (0x01)
//
// wifi_promiscuous_pkt_t layout:
//   [rx_ctrl : wifi_pkt_rx_ctrl_t][payload : uint8_t[]]
//   rx_ctrl.rssi = RSSI dalam dBm (negatif)
//
// ⚠️  Callback ini berjalan di WiFi task context — SANGAT singkat
// =============================================================================
void EspNowMesh::_promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    // Hanya proses data frame
    if (type != WIFI_PKT_DATA) return;
    if (!_instance) return;

    const wifi_promiscuous_pkt_t* pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);

    // ESP-NOW payload dimulai setelah 802.11 header (24 byte) +
    // LLC header (8 byte) + ESP-NOW vendor action header (7 byte) = 39 byte
    // Tapi panjang bisa bervariasi — kita cari magic byte ESP-NOW
    // Cara lebih andal: cek apakah src MAC = GATEWAY MAC
    const uint8_t* payload = pkt->payload;
    const uint16_t payloadLen = pkt->rx_ctrl.sig_len;

    // Filter sederhana: cari byte BEACON (0x01) di offset yang diketahui
    // Offset 39 = 24 (802.11 hdr) + 8 (LLC) + 7 (ESP-NOW action hdr)
    // Ini bisa bervariasi, tapi untuk ESP-NOW di channel fixed ini cukup andal
    static constexpr uint16_t ESPNOW_PAYLOAD_OFFSET = 39;

    if (payloadLen <= ESPNOW_PAYLOAD_OFFSET) return;

    const uint8_t firstByte = payload[ESPNOW_PAYLOAD_OFFSET];
    if (firstByte != static_cast<uint8_t>(PacketType::BEACON)) return;

    // Sync channel jika beacon datang dari channel berbeda
    const uint8_t beaconCh = pkt->rx_ctrl.channel;
    if (beaconCh > 0 && beaconCh <= 13 && beaconCh != s_channel)
    {
        s_channel = beaconCh;
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        LOG_INFO("MESH", "Channel sync via beacon: %d", s_channel);
    }

    _instance->_lastBeaconRssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
}
