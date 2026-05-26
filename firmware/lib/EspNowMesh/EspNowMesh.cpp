// File: firmware/lib/EspNowMesh/EspNowMesh.cpp
// =============================================================================
// v5.0 — WiFi-Channel-Sync
//
// KONSEP BARU (menggantikan promiscuous scan v4.x):
//   Sensor node konek ke WiFi AP yang SAMA dengan gateway untuk mendapatkan
//   channel yang tepat, lalu langsung disconnect dan init ESP-NOW di channel itu.
//
// ALASAN GANTI:
//   Promiscuous scan (v3.x–v4.x) terlalu sensitif terhadap timing — sensor
//   harus boot saat gateway sudah memancarkan beacon di channel yang benar.
//   WiFi association jauh lebih reliable: channel AP selalu konsisten.
//
// FLOW SENSOR (baru):
//   1. WiFi.begin(SSID, PASSWORD)          ← connect ke AP yang sama dg gateway
//   2. Tunggu WL_CONNECTED (max 10 detik)
//   3. ch = WiFi.channel()                 ← baca channel AP
//   4. WiFi.disconnect() + WiFi.mode(OFF)  ← keluar dari WiFi
//   5. delay(200)                          ← tunggu radio stabil
//   6. WiFi.mode(STA) + set ch             ← siapkan mode ESP-NOW
//   7. esp_now_init()                      ← init ESP-NOW di channel yang benar
//
// FLOW GATEWAY (tidak berubah):
//   WiFi tetap tersambung ke AP (STA+AP) — channel dikunci oleh koneksi STA.
//   Beacon dikirim di channel yang sama secara otomatis.
//
// TRADEOFF:
//   + Tidak ada timing sensitivity sama sekali
//   + Tidak ada promiscuous mode, tidak ada background task discovery
//   + Sensor boot order bebas (bisa sebelum/sesudah gateway)
//   - Sensor butuh credentials WiFi (sudah ada di credentials.h)
//   - WiFi connect +1-3 detik di awal boot (sekali saja)

#include <esp_wifi.h>
#include "EspNowMesh.h"

static constexpr char TAG[] = "MESH";

static uint8_t  s_channel          = 1;
static bool     s_channelConfirmed = false;
static volatile bool s_espnowReady = false;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

QueueHandle_t g_rawQueue = nullptr;
extern QueueHandle_t g_mqttQueue;

EspNowMesh* EspNowMesh::_instance = nullptr;


// =============================================================================
// _updateAllPeerChannels() — Update channel semua peer terdaftar
// HANYA boleh dipanggil setelah esp_now_init()!
// =============================================================================
static void _updateAllPeerChannels(uint8_t newChannel)
{
    if (!s_espnowReady) return;  // guard: jangan panggil sebelum esp_now_init

    esp_now_peer_info_t peer{};
    esp_err_t err = esp_now_fetch_peer(true, &peer);
    uint8_t updated = 0;

    while (err == ESP_OK)
    {
        if (peer.channel != newChannel)
        {
            peer.channel = newChannel;
            if (esp_now_mod_peer(&peer) == ESP_OK) updated++;
        }
        err = esp_now_fetch_peer(false, &peer);
    }

    if (updated > 0)
        LOG_INFO(TAG, "Peers diupdate ke ch=%d (%d peer)", newChannel, updated);
}


// =============================================================================
// begin()
// =============================================================================
bool EspNowMesh::begin(bool senderMode)
{
    _instance     = this;
    _senderMode   = senderMode;
    s_espnowReady = false;

    if (senderMode)
    {
        // ── SENSOR: WiFi-Channel-Sync (v5.0) ─────────────────────────────────
        // Konek ke WiFi AP yang sama dengan gateway untuk mendapatkan channel.
        // Setelah dapat channel, langsung disconnect dan init ESP-NOW.
        // Tidak ada timing sensitivity — WiFi association selalu reliable.

        LOG_INFO(TAG, "Sensor: WiFi channel-sync dimulai | SSID='%s'", Wifi::SSID);

        WiFi.mode(WIFI_STA);
        WiFi.begin(Wifi::SSID, Wifi::PASSWORD);

        const uint32_t wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED)
        {
            if (millis() - wifiStart > Timing::WIFI_TIMEOUT_MS)
            {
                LOG_ERROR(TAG, "WiFi timeout setelah %lu ms! Pakai ch=1 sebagai fallback.",
                          Timing::WIFI_TIMEOUT_MS);
                s_channel = 1;
                goto wifi_done;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // Berhasil konek — baca channel dari AP
        s_channel = static_cast<uint8_t>(WiFi.channel());
        LOG_INFO(TAG, "WiFi connected! ch=%d | IP=%s | RSSI=%d dBm",
                 s_channel, WiFi.localIP().toString().c_str(), WiFi.RSSI());

    wifi_done:
        // Keluar dari WiFi — radio akan dipakai oleh ESP-NOW
        WiFi.disconnect(true);      // true = juga erase credentials dari NVS
        WiFi.mode(WIFI_OFF);
        vTaskDelay(pdMS_TO_TICKS(300));  // beri waktu radio reset

        // Siapkan mode STA untuk ESP-NOW
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));

        // Set channel sesuai yang didapat dari WiFi AP
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        s_channelConfirmed = true;  // channel sudah pasti, tidak perlu discovery

        LOG_INFO(TAG, "Sensor: ESP-NOW akan init di ch=%d (dari WiFi AP)", s_channel);
    }
    else
    {
        // ── GATEWAY: WiFi sudah di-init oleh NetworkMqtt::begin() ────────────
        // JANGAN panggil WiFi.mode() lagi — akan reset state WiFi/ESP-NOW.
        uint8_t ch = 0;
        wifi_second_chan_t sch;
        if (esp_wifi_get_channel(&ch, &sch) == ESP_OK && ch > 0 && ch <= 13)
            s_channel = ch;
        else
            s_channel = 1;

        s_channelConfirmed = true;
        LOG_INFO(TAG, "Gateway: ch=%d dari WiFi STA aktif", s_channel);
    }

    // ── ESP-NOW init ─────────────────────────────────────────────────────────
    if (esp_now_init() != ESP_OK)
    {
        LOG_ERROR(TAG, "esp_now_init() gagal!");
        return false;
    }

    s_espnowReady = true;

    esp_now_register_send_cb(_onDataSent);
    esp_now_register_recv_cb(_onDataRecv);

    // ── Register peers ───────────────────────────────────────────────────────
    // Dengan dynamic peer di _send(), peer awal ini tetap didaftarkan
    // untuk BROADCAST dan sebagai fallback.
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

        // v5.0: tidak ada background discovery task — channel sudah pasti
        LOG_INFO(TAG, "Mode: SENSOR | MAC: %s | ch=%d | channelConfirmed=YES",
                 WiFi.macAddress().c_str(), s_channel);
    }
    else
    {
        if (!_addPeer(MacAddr::NODE_A)) return false;
        if (!_addPeer(MacAddr::NODE_B)) return false;
        if (!_addPeer(BROADCAST_MAC))   return false;
        LOG_INFO(TAG, "Mode: GATEWAY | MAC: %s | ch=%d",
                 WiFi.macAddress().c_str(), s_channel);
    }

    // Aktifkan promiscuous mode untuk RSSI monitoring (tidak untuk discovery)
    if (senderMode)
    {
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(_promiscuousRxCb);
        LOG_INFO(TAG, "Promiscuous ON untuk RSSI monitoring");
    }

    return true;
}


// =============================================================================
// setGatewayChannel() — Dipanggil dari main.cpp setelah WiFi connect
// =============================================================================
void EspNowMesh::setGatewayChannel(uint8_t channel)
{
    if (channel == 0 || channel > 13) return;

    const uint8_t oldCh = s_channel;
    s_channel           = channel;
    s_channelConfirmed  = true;

    // Update channel radio (untuk gateway dalam AP_STA, STA yang kontrol channel,
    // tapi explicit set memastikan sinkronisasi internal s_channel)
    // esp_wifi_set_channel tidak dipakai di gateway karena channel dikontrol STA.

    _updateAllPeerChannels(s_channel);

    LOG_INFO(TAG, "setGatewayChannel: %d → %d | peers updated", oldCh, s_channel);
}


// =============================================================================
// isChannelConfirmed()
// =============================================================================
bool EspNowMesh::isChannelConfirmed() const { return s_channelConfirmed; }


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
// sendCsAxis() / sendCsPpg() / forwardRoutedCs() / sendCombined() / sendHeartbeat()
// (tidak ada perubahan dari v3.3)
// =============================================================================
bool EspNowMesh::sendCsAxis(uint8_t pktType, uint8_t nodeId,
                             const float y[CS_M], bool fingerOn,
                             uint32_t timestamp, const uint8_t* dstMac)
{
    CS1AxisPacket pkt{};
    pkt.header = { static_cast<PacketType>(pktType), nodeId, timestamp };
    memcpy(pkt.y, y, CS_M * sizeof(float));
    pkt.edge   = { fingerOn, 0 };
    // vTaskDelay(1ms) dihapus v5.1 — eliminasi 7ms blocking per window
    return _send(&pkt, sizeof(CS1AxisPacket), dstMac);
}

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

bool EspNowMesh::forwardRoutedCs(uint8_t relayNodeId, uint8_t originalNodeId,
                                  const uint8_t* innerData, uint8_t innerLen)
{
    if (innerLen > sizeof(RoutedCsPacket::inner)) return false;
    RoutedCsPacket pkt{};
    pkt.header.type           = PacketType::ROUTED_CS;
    pkt.header.relayNodeId    = relayNodeId;
    pkt.header.originalNodeId = originalNodeId;
    pkt.header.innerLen       = innerLen;
    pkt.header.relayTimestamp = static_cast<uint32_t>(millis());
    memcpy(pkt.inner, innerData, innerLen);
    return _send(&pkt, sizeof(RoutedCsHeader) + innerLen, MacAddr::GATEWAY);
}

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
// _send() — Smart peer channel management (v5.1)
//
// STRATEGI LAMA (v4.1): del+add peer setiap kirim → 14 mutex ops/window extra.
// STRATEGI BARU (v5.1): peer sudah terdaftar via _addPeer() saat begin().
//   Saat _send(), cek apakah channel peer perlu diupdate:
//   - Jika sama → langsung send (0 extra ops)
//   - Jika berbeda → esp_now_mod_peer() sekali, lalu send
//
// Dengan v5.0 (WiFi channel sync), channel TIDAK PERNAH berubah setelah boot.
// Jadi normal path = 0 extra ops per send.
// Broadcast MAC tidak perlu channel management.
// =============================================================================
static uint8_t s_lastSentChannel = 0;  // track channel terakhir yang dipakai

bool EspNowMesh::_send(const void* data, size_t len, const uint8_t* dstMac)
{
    // Broadcast: tidak butuh channel management
    const bool isBroadcast = (dstMac[0] == 0xFF && dstMac[1] == 0xFF &&
                               dstMac[2] == 0xFF && dstMac[3] == 0xFF &&
                               dstMac[4] == 0xFF && dstMac[5] == 0xFF);

    if (!isBroadcast && s_lastSentChannel != s_channel)
    {
        // Channel berubah sejak send terakhir — update semua peer terdaftar
        // (ini jarang terjadi di v5.0, hanya jika gateway roam channel)
        _updateAllPeerChannels(s_channel);
        s_lastSentChannel = s_channel;
        LOG_WARN(TAG, "Peer channel diupdate ke ch=%d", s_channel);
    }

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
// _promiscuousRxCb() — ATURAN KETAT:
//
//   BOLEH:   update s_channel, s_channelConfirmed, _lastBeaconRssi
//            update g_routerPtr->updateSelfRssi() (hanya write ke variable)
//            panggil esp_wifi_set_channel()
//
//   DILARANG: esp_now_fetch_peer(), esp_now_mod_peer(), esp_now_add_peer()
//             — semua fungsi ini akan crash jika esp_now belum init
//             — bahkan setelah esp_now init, TIDAK AMAN dipanggil dari ISR
//
//   Setelah esp_now ready (s_espnowReady=true), channel sync dilakukan
//   via flag s_needChannelUpdate yang diproses dari task context.
// =============================================================================

// Flag untuk deferred channel update (diproses dari task, bukan ISR)
static volatile uint8_t  s_pendingChannel = 0;
static volatile bool     s_needChannelUpdate = false;

bool EspNowMesh::processPendingChannelSync()
{
    if (!s_needChannelUpdate) return false;

    const uint8_t newCh = s_pendingChannel;
    s_needChannelUpdate = false;

    esp_wifi_set_channel(newCh, WIFI_SECOND_CHAN_NONE);
    s_channel = newCh;

    // Re-register gateway peer — handles both channel change AND gateway restart
    // esp_now_del_peer + esp_now_add_peer = fresh registration
    esp_now_del_peer(MacAddr::GATEWAY);
    _addPeer(MacAddr::GATEWAY);   // _addPeer sudah handle duplicate check

    // Update channel semua peer lain
    esp_now_peer_info_t peer{};
    esp_err_t err = esp_now_fetch_peer(true, &peer);
    uint8_t updated = 0;
    while (err == ESP_OK)
    {
        if (peer.channel != newCh)
        {
            peer.channel = newCh;
            if (esp_now_mod_peer(&peer) == ESP_OK) updated++;
        }
        err = esp_now_fetch_peer(false, &peer);
    }

    LOG_WARN(TAG, "Channel sync + peer re-register → ch=%d | %d peers",
             newCh, updated);
    return true;
}


// =============================================================================
// _taskChannelDiscovery() — DIHAPUS di v5.0
//
// Digantikan oleh WiFi-channel-sync di begin().
// Fungsi ini dikosongkan dan tidak dipanggil di manapun.
// Tetap ada untuk menghindari linker error jika ada referensi tersisa.
// =============================================================================
void EspNowMesh::_taskChannelDiscovery(void* param)
{
    // Tidak dipakai di v5.0 — channel sudah diketahui via WiFi AP
    LOG_WARN("DISC", "_taskChannelDiscovery dipanggil tapi tidak seharusnya di v5.0!");
    vTaskDelete(NULL);
}


void EspNowMesh::_promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    if (!_instance) return;

    const wifi_promiscuous_pkt_t* ppkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);

    const uint8_t*  payload    = ppkt->payload;
    const uint16_t  payloadLen = ppkt->rx_ctrl.sig_len;

    if (payloadLen < 50) return;

    // Cek apakah frame dari gateway MAC (transmitter addr di byte 10..15)
    bool fromGateway = (
        payload[10] == MacAddr::GATEWAY[0] &&
        payload[11] == MacAddr::GATEWAY[1] &&
        payload[12] == MacAddr::GATEWAY[2] &&
        payload[13] == MacAddr::GATEWAY[3] &&
        payload[14] == MacAddr::GATEWAY[4] &&
        payload[15] == MacAddr::GATEWAY[5]
    );

    if (!fromGateway) return;

    const uint8_t beaconCh = ppkt->rx_ctrl.channel;
    const int8_t  rssi     = static_cast<int8_t>(ppkt->rx_ctrl.rssi);

    if (beaconCh < 1 || beaconCh > 13) return;

    // Update RSSI — aman dari ISR (hanya write ke volatile variable)
    _instance->_lastBeaconRssi = rssi;

    // Update router — hanya write ke int8_t dengan critical section
    if (g_routerPtr != nullptr)
        g_routerPtr->updateSelfRssi(rssi);

    // --- Deteksi gateway restart ---
    // Jika beacon sempat hilang lama lalu muncul lagi, gateway kemungkinan restart.
    // Set flag untuk re-register peer dari task context.
    static uint32_t s_lastBeaconMs   = 0;
    static bool     s_gatewayWasLost = false;

    const uint32_t nowMs = millis();

    if (s_lastBeaconMs > 0 &&
        (nowMs - s_lastBeaconMs) > 5000)   // beacon hilang > 5 detik
    {
        s_gatewayWasLost = true;
    }

    s_lastBeaconMs = nowMs;

    // Jika gateway baru muncul lagi setelah hilang → flag re-register
    if (s_gatewayWasLost && s_espnowReady)
    {
        s_gatewayWasLost    = false;
        s_needChannelUpdate = true;   // trigger processPendingChannelSync()
        s_pendingChannel    = beaconCh;
    }

    // ── Channel sync logic ────────────────────────────────────────────────────
    // v5.0: s_channelConfirmed selalu true sejak begin() karena channel
    // sudah diketahui via WiFi AP. Blok ini hanya untuk deteksi channel
    // drift saat operasional (gateway roam ke channel lain — jarang terjadi).
    if (!s_channelConfirmed)
    {
        // Seharusnya tidak masuk sini di v5.0
        // Jika masuk, artinya WiFi connect gagal dan fallback ke ch=1
        if (rssi < -85) return;

        s_channel          = beaconCh;
        s_channelConfirmed = true;
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

        LOG_WARN(TAG, "[v5.0 fallback] Channel dikunci dari beacon: ch=%d", s_channel);

        if (s_espnowReady)
        {
            s_pendingChannel    = beaconCh;
            s_needChannelUpdate = true;
        }
    }
    else if (s_espnowReady && beaconCh != s_channel)
    {
        // Fase operasional (esp_now sudah init): channel berubah
        // JANGAN panggil esp_now_mod_peer dari ISR!
        // Set flag, biarkan task yang handle
        static uint8_t s_midCandCh    = 0;
        static uint8_t s_midCandCount = 0;

        if (beaconCh != s_midCandCh) { s_midCandCh = beaconCh; s_midCandCount = 1; }
        else                           s_midCandCount++;

        if (s_midCandCount >= 3)
        {
            s_pendingChannel    = beaconCh;
            s_needChannelUpdate = true;   // ← task akan proses ini
            s_midCandCh    = 0;
            s_midCandCount = 0;
        }
    }
}