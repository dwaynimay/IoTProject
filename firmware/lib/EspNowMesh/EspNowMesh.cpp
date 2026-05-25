// File: firmware/lib/EspNowMesh/EspNowMesh.cpp
// =============================================================================
// PERBAIKAN v3.3 — Channel Sync Fix
//
// MASALAH:
//   Sensor sweep 8 detik tapi gateway beacon belum ada di channel yang benar
//   (gateway baru connect WiFi ch=2 setelah ~2 detik, tapi sensor sudah
//   timeout sweep di ch lain → peer terdaftar di channel yang salah →
//   NACK 100% selamanya).
//
// FIX:
//   1. Sensor: setelah sweep timeout, JANGAN stop scan. Jalankan
//      taskChannelSync (background task) yang terus sweep semua channel
//      sampai beacon ditemukan, lalu re-register peer dengan channel baru.
//
//   2. Sensor: _promiscuousRxCb saat deteksi beacon di channel berbeda,
//      langsung re-register semua peer ke channel baru (esp_now_mod_peer).
//
//   3. Gateway: tidak ada perubahan logika, tapi tambah log channel saat
//      beacon dikirim untuk memudahkan debug.
// =============================================================================

#include <esp_wifi.h>
#include "EspNowMesh.h"

static constexpr char TAG[] = "MESH";

static uint8_t  s_channel          = 1;
static bool     s_channelConfirmed = false; // true setelah beacon diterima

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

QueueHandle_t g_rawQueue = nullptr;
extern QueueHandle_t g_mqttQueue;

EspNowMesh* EspNowMesh::_instance = nullptr;


// =============================================================================
// _updateAllPeerChannels() — Update channel semua peer terdaftar
// Dipanggil saat channel berubah (baik di sensor maupun gateway).
// =============================================================================
static void _updateAllPeerChannels(uint8_t newChannel)
{
    // Iterasi semua peer yang terdaftar dan update channel-nya
    esp_now_peer_info_t peer{};
    esp_err_t err = esp_now_fetch_peer(true, &peer); // first=true: mulai dari awal

    uint8_t updated = 0;
    while (err == ESP_OK)
    {
        if (peer.channel != newChannel)
        {
            peer.channel = newChannel;
            if (esp_now_mod_peer(&peer) == ESP_OK)
                updated++;
        }
        err = esp_now_fetch_peer(false, &peer); // false: lanjut ke berikutnya
    }

    if (updated > 0)
        LOG_INFO(TAG, "Semua peer diupdate ke ch=%d (%d peer)", newChannel, updated);
}


// =============================================================================
// begin()
// =============================================================================
bool EspNowMesh::begin(bool senderMode)
{
    _instance   = this;
    _senderMode = senderMode;

    // [WAJIB] WiFi.mode() harus dipanggil SEBELUM esp_now_init()
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    if (senderMode)
    {
        // ── Sensor node: aktifkan promiscuous untuk RSSI + channel detection ─
        s_channel          = 0;
        s_channelConfirmed = false;

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(_promiscuousRxCb);

        // Mulai di ch=1, biarkan _promiscuousRxCb yang deteksi beacon
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

        LOG_INFO(TAG, "Sensor: promiscuous ON, sweep ch=1..13 (timeout=%d ms)",
                 RoutingCfg::CHANNEL_SWEEP_TIMEOUT_MS);

        LOG_INFO(TAG, "Sensor: mencari beacon gateway (tanpa batas waktu)...");

        uint32_t sweepRound = 0;

        while (!s_channelConfirmed)
        {
            sweepRound++;

            for (uint8_t tryC = 1; tryC <= 13 && !s_channelConfirmed; tryC++)
            {
                esp_wifi_set_channel(tryC, WIFI_SECOND_CHAN_NONE);
                delay(400); // 400ms per channel = cukup tangkap 1 beacon
            }

            // Log progress setiap 3 putaran (~16 detik sekali)
            if (sweepRound % 3 == 0)
            {
                LOG_WARN(TAG,
                         "Sweep ronde #%lu selesai — beacon belum ditemukan. "
                         "Pastikan gateway nyala.",
                         sweepRound);
            }
        }

        LOG_INFO(TAG, "Beacon ditemukan! ch=%d | lanjut inisialisasi...",
                 s_channel);
    }
    else
    {
        // ── Gateway node ──────────────────────────────────────────────────────
        s_channel          = 1; // sementara, update via setGatewayChannel()
        s_channelConfirmed = false;
        LOG_INFO(TAG, "Gateway WiFi.mode(STA) OK | ch=%d sementara", s_channel);
    }

    // ── esp_now_init ──────────────────────────────────────────────────────────
    if (esp_now_init() != ESP_OK)
    {
        LOG_ERROR(TAG, "esp_now_init() gagal!");
        return false;
    }

    esp_now_register_send_cb(_onDataSent);
    esp_now_register_recv_cb(_onDataRecv);

    // ── Register peers ────────────────────────────────────────────────────────
    if (senderMode)
    {
        if (!_addPeer(MacAddr::GATEWAY)) return false;

        #if NODE_ID == 1
            if (!_addPeer(MacAddr::NODE_B)) return false;
            LOG_INFO(TAG, "Sensor %d: GATEWAY + NODE_B | ch=%d", NODE_ID, s_channel);
        #else
            if (!_addPeer(MacAddr::NODE_A)) return false;
            LOG_INFO(TAG, "Sensor %d: GATEWAY + NODE_A | ch=%d", NODE_ID, s_channel);
        #endif

        if (!_addPeer(BROADCAST_MAC)) return false;
        LOG_INFO(TAG, "Mode: SENSOR | MAC: %s | ch=%d | confirmed=%s",
                 WiFi.macAddress().c_str(), s_channel,
                 s_channelConfirmed ? "Y" : "N (akan auto-sync)");
    }
    else
    {
        if (!_addPeer(MacAddr::NODE_A)) return false;
        if (!_addPeer(MacAddr::NODE_B)) return false;
        if (!_addPeer(BROADCAST_MAC))   return false;
        LOG_INFO(TAG, "Mode: GATEWAY | MAC: %s | ch=%d sementara",
                 WiFi.macAddress().c_str(), s_channel);
    }

    return true;
}


// =============================================================================
// setGatewayChannel() — Dipanggil dari main.cpp setelah WiFi connect
// =============================================================================
void EspNowMesh::setGatewayChannel(uint8_t channel)
{
    if (channel == 0 || channel > 13) return;
    if (channel == s_channel) return;

    const uint8_t oldCh = s_channel;
    s_channel = channel;

    const uint8_t *peers[] = {MacAddr::NODE_A, MacAddr::NODE_B, BROADCAST_MAC};
    uint8_t updated = 0;

    for (const uint8_t *mac : peers)
    {
        esp_now_peer_info_t info{};
        memcpy(info.peer_addr, mac, 6);
        info.channel = s_channel;
        info.encrypt = false;
        
        // LANGSUNG mod_peer, abaikan get_peer.
        // Jika belum terdaftar, mod_peer otomatis akan mengembalikan fail, tapi aman.
        if (esp_now_mod_peer(&info) == ESP_OK) {
            updated++;
        }
    }

    LOG_INFO(TAG, "Channel updated: %d → %d | %d peer diupdate",
             oldCh, s_channel, updated);
}

// =============================================================================
// Status
// =============================================================================
bool EspNowMesh::isChannelConfirmed() const
{
    return s_channelConfirmed;
}

// =============================================================================
// sendBeacon()
// =============================================================================
bool EspNowMesh::sendBeacon()
{
    BeaconPacket pkt{};
    pkt.header = { PacketType::BEACON, RoutingCfg::GATEWAY_NODE_ID,
                   static_cast<uint32_t>(millis()) };
    pkt.seqNum = _beaconSeqNum++;

    const bool ok = _send(&pkt, sizeof(BeaconPacket), BROADCAST_MAC);
    LOG_DEBUG(TAG, "Beacon #%d ch=%d | ok=%s", pkt.seqNum, s_channel,
              ok ? "Y" : "N");
    return ok;
}


// =============================================================================
// sendRssiReport()
// =============================================================================
bool EspNowMesh::sendRssiReport(uint8_t selfNodeId, int8_t rssiToGateway)
{
    RssiReportPacket pkt{};
    pkt.header        = { PacketType::RSSI_REPORT, selfNodeId,
                          static_cast<uint32_t>(millis()) };
    pkt.rssiToGateway = rssiToGateway;
    pkt.hopCount      = 1;
    pkt.reserved      = 0;

    #if NODE_ID == 1
        const uint8_t* neighborMac = MacAddr::NODE_B;
    #else
        const uint8_t* neighborMac = MacAddr::NODE_A;
    #endif
    return _send(&pkt, sizeof(RssiReportPacket), neighborMac);
}


// =============================================================================
// sendCsAxis()
// =============================================================================
bool EspNowMesh::sendCsAxis(uint8_t pktType, uint8_t nodeId,
                             const float y[CS_M], bool fingerOn,
                             uint32_t timestamp, const uint8_t* dstMac)
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
// sendCsPpg()
// =============================================================================
bool EspNowMesh::sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                            int8_t heartRate, bool ppgValid, float spo2,
                            bool fingerOn, uint32_t timestamp,
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
// forwardRoutedCs()
// =============================================================================
bool EspNowMesh::forwardRoutedCs(uint8_t relayNodeId, uint8_t originalNodeId,
                                  const uint8_t* innerData, uint8_t innerLen)
{
    if (innerLen > sizeof(RoutedCsPacket::inner))
    {
        LOG_ERROR(TAG, "forwardRoutedCs: innerLen=%d > max=%d",
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
    return _send(&pkt, sizeof(RoutedCsHeader) + innerLen, MacAddr::GATEWAY);
}


// =============================================================================
// sendCombined() + sendHeartbeat()
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
    pkt.rssi    = static_cast<uint8_t>(-_lastBeaconRssi);
    return _send(&pkt, sizeof(HeartbeatPacket), MacAddr::GATEWAY);
}


// =============================================================================
// _send()
// =============================================================================
bool EspNowMesh::_send(const void* data, size_t len, const uint8_t* dstMac)
{
    const esp_err_t err = esp_now_send(
        dstMac, reinterpret_cast<const uint8_t*>(data), len);
    if (err != ESP_OK)
        LOG_EVERY_N(10, LOG_WARN, TAG, "esp_now_send err: 0x%X", err);
    _lastSendOk = (err == ESP_OK);
    return _lastSendOk;
}

bool EspNowMesh::_addPeer(const uint8_t* mac)
{
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
    LOG_DEBUG(TAG, "Peer OK: %02X:%02X:%02X ch=%d",
              mac[0], mac[1], mac[2], s_channel);
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
        LOG_EVERY_N(5, LOG_WARN, "MESH", "NACK total=%lu rate=%.1f%%",
                    _nackCount, 100.0f * _nackCount / (_ackCount + _nackCount));
    }
}

void EspNowMesh::_onDataRecv(const uint8_t* mac, const uint8_t* data, int len)
{
    if (len < 1) return;
    const uint8_t pktType = data[0];

    if (pktType == static_cast<uint8_t>(PacketType::RSSI_REPORT))
    {
        if (len >= static_cast<int>(sizeof(RssiReportPacket)) && g_routerPtr)
        {
            const auto* pkt = reinterpret_cast<const RssiReportPacket*>(data);
            g_routerPtr->updateNeighborRssi(pkt->header.nodeId,
                                            pkt->rssiToGateway);
        }
        return;
    }

    if (!g_rawQueue) return;

    RawPacket raw{};
    raw.len = static_cast<uint8_t>(len <= 250 ? len : 250);
    memcpy(raw.data,   data, raw.len);
    memcpy(raw.srcMac, mac,  6);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(g_rawQueue, &raw, &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

// =============================================================================
// _promiscuousRxCb() — Sensor: deteksi beacon & auto-sync channel
//
// KUNCI FIX: saat beacon terdeteksi di channel berbeda dari s_channel,
// langsung update s_channel DAN re-register semua peer via _updateAllPeerChannels().
// Tanpa re-register peer, esp_now_send() akan tetap NACK karena peer
// masih tercatat di channel lama.
// =============================================================================
void EspNowMesh::_promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    if (!_instance) return;

    const wifi_promiscuous_pkt_t* ppkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);

    const uint8_t*  payload    = ppkt->payload;
    const uint16_t  payloadLen = ppkt->rx_ctrl.sig_len;

    if (payloadLen < 50) return;

    // ── Cek apakah ini frame dari gateway MAC ─────────────────────────────
    // 802.11 data frame: transmitter address ada di byte 10..15
    // Bandingkan dengan MacAddr::GATEWAY
    bool fromGateway = (
        payload[10] == MacAddr::GATEWAY[0] &&
        payload[11] == MacAddr::GATEWAY[1] &&
        payload[12] == MacAddr::GATEWAY[2] &&
        payload[13] == MacAddr::GATEWAY[3] &&
        payload[14] == MacAddr::GATEWAY[4] &&
        payload[15] == MacAddr::GATEWAY[5]
    );

    if (!fromGateway) return;  // bukan dari gateway, skip

    // ── Frame dari gateway ditemukan — ini sudah cukup untuk sync channel!
    // Tidak perlu parse isi beacon, cukup tahu frame dari gateway ada di channel ini
    const uint8_t beaconCh = ppkt->rx_ctrl.channel;
    const int8_t  rssi     = static_cast<int8_t>(ppkt->rx_ctrl.rssi);

    if (beaconCh < 1 || beaconCh > 13) return;

    // Update RSSI
    _instance->_lastBeaconRssi = rssi;

    // Update router langsung
    if (g_routerPtr != nullptr)
        g_routerPtr->updateSelfRssi(rssi);

    // ── Channel sync ──────────────────────────────────────────────────────
    if (beaconCh != s_channel)
    {
        static uint8_t s_candCh    = 0;
        static uint8_t s_candCount = 0;

        if (beaconCh != s_candCh)
        {
            s_candCh    = beaconCh;
            s_candCount = 1;
        }
        else
        {
            s_candCount++;
        }

        if (s_candCount >= 3)
        {
            const uint8_t oldCh = s_channel;
            s_channel           = beaconCh;
            s_channelConfirmed  = true;
            s_candCh            = 0;
            s_candCount         = 0;

            esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
            _updateAllPeerChannels(s_channel);

            LOG_WARN("MESH", "Channel sync: %d → %d | RSSI=%d dBm | peers updated",
                     oldCh, s_channel, (int)rssi);
        }
    }
    else
    {
        // Channel sudah benar
        if (!s_channelConfirmed)
        {
            s_channelConfirmed = true;
            _updateAllPeerChannels(s_channel);

            LOG_INFO("MESH", "Channel %d confirmed dari gateway frame | RSSI=%d dBm",
                     s_channel, (int)rssi);
        }

        static uint8_t s_candCh    = 0;
        static uint8_t s_candCount = 0;
        s_candCh    = 0;
        s_candCount = 0;
    }
}