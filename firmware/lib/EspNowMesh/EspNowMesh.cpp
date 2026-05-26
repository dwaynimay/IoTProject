// File: firmware/lib/EspNowMesh/EspNowMesh.cpp
// =============================================================================
// PERBAIKAN v4.0 — Boot-Anytime: sensor boot non-blocking,
//                  channel discovery di background FreeRTOS task.
//                  Gateway dan sensor bisa dinyalakan di waktu berbeda.
// =============================================================================
// PERBAIKAN v3.4 — Fix urutan init: promiscuous sweep SELESAI dulu,
//                  baru esp_now_init(). Callback tidak boleh sentuh ESP-NOW API.
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
static bool     s_channelConfirmed = false;

// Flag khusus: apakah esp_now sudah di-init?
// Dipakai oleh _promiscuousRxCb untuk guard sebelum sentuh ESP-NOW API.
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
    _instance   = this;
    _senderMode = senderMode;
    s_espnowReady = false;

    if (senderMode)
    {
        // ── FASE 1: Init WiFi STA — channel discovery di background ──────────
        // ESP-NOW langsung di-init dengan ch=1 (default).
        // Background task akan sweep channel sampai beacon gateway ditemukan,
        // lalu update peer channel via processPendingChannelSync().
        // Sensor bisa boot TANPA gateway — discovery task terus sweep.
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        s_channel          = 1;     // Default, akan diupdate oleh discovery
        s_channelConfirmed = false;

        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

        LOG_INFO(TAG, "Sensor: init di ch=%d | discovery akan jalan di background",
                 s_channel);
    }
    else
    {
        // ── Gateway: WiFi sudah di-init oleh NetworkMqtt::begin() ────────────
        // JANGAN panggil WiFi.mode() lagi — akan reset state WiFi/ESP-NOW.
        uint8_t ch = 0;
        wifi_second_chan_t sch;
        if (esp_wifi_get_channel(&ch, &sch) == ESP_OK && ch > 0 && ch <= 13)
            s_channel = ch;
        else
            s_channel = 1;

        s_channelConfirmed = false;
        LOG_INFO(TAG, "Gateway: menggunakan ch=%d dari WiFi aktif", s_channel);
    }

    // ── FASE 3: esp_now_init() — setelah channel pasti ───────────────────────
    if (esp_now_init() != ESP_OK)
    {
        LOG_ERROR(TAG, "esp_now_init() gagal!");
        return false;
    }

    s_espnowReady = true;  // ← set SETELAH esp_now_init() berhasil

    esp_now_register_send_cb(_onDataSent);
    esp_now_register_recv_cb(_onDataRecv);

    // ── FASE 4: Register peers ────────────────────────────────────────────────
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

        // ── FASE 5: Start background discovery task ──────────────────────────
        // Discovery task sweep channel sampai beacon gateway ditemukan.
        // Promiscuous mode dikelola oleh task, bukan begin().
        // Task sensor lain (IMU, PPG) bisa jalan parallel.
        // taskCSSender dan taskRssiExchange menunggu isChannelConfirmed().
        xTaskCreatePinnedToCore(_taskChannelDiscovery, "DISC", 3072,
                                nullptr, 2, nullptr, 0);

        LOG_INFO(TAG, "Mode: SENSOR | MAC: %s | ch=%d | discovery=BACKGROUND",
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
    const bool ok = _send(&pkt, sizeof(CS1AxisPacket), dstMac);
    vTaskDelay(pdMS_TO_TICKS(1));
    return ok;
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
// _send() / _addPeer() / _isPeerRegistered()
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
// _taskChannelDiscovery() — Background FreeRTOS task (v4.0 Boot-Anytime)
//
// Menggantikan blocking sweep di begin(). Task ini:
//   1. Enable promiscuous mode
//   2. Sweep channel 1..13 sampai beacon gateway ditemukan
//   3. _promiscuousRxCb set s_channelConfirmed + s_needChannelUpdate
//   4. processPendingChannelSync() di taskCSSender handle peer re-register
//   5. Promiscuous tetap ON untuk RSSI monitoring setelah discovery
//   6. Task delete diri sendiri
//
// Sensor bisa boot TANPA gateway — task ini terus sweep tanpa batas.
// =============================================================================
void EspNowMesh::_taskChannelDiscovery(void* param)
{
    static constexpr char DTAG[] = "DISC";

    // Dwell time bervariasi untuk memecah phase lock dengan beacon gateway.
    // Jika dwell FIXED (mis. 400ms) dan beacon FIXED (1000ms), keduanya
    // bisa "saling menghindari" untuk waktu yang lama (phase lock problem).
    // Randomisasi memastikan phase bergeser setiap channel → pasti ketemu.
    static constexpr uint32_t DWELL_MIN_MS = 200;
    static constexpr uint32_t DWELL_MAX_MS = 600;

    LOG_INFO(DTAG, "Background channel discovery dimulai | "
             "dwell=%lu-%lu ms (random anti-phase-lock)",
             DWELL_MIN_MS, DWELL_MAX_MS);

    // Enable promiscuous mode untuk deteksi beacon gateway
    // Aman: esp_now sudah init, callback hanya update variable
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(_promiscuousRxCb);

    uint32_t sweepRound = 0;

    while (!s_channelConfirmed)
    {
        sweepRound++;
        for (uint8_t tryC = 1; tryC <= 13 && !s_channelConfirmed; tryC++)
        {
            esp_wifi_set_channel(tryC, WIFI_SECOND_CHAN_NONE);

            // Dwell time acak setiap channel → pecah phase lock
            const uint32_t dwell = DWELL_MIN_MS +
                (esp_random() % (DWELL_MAX_MS - DWELL_MIN_MS + 1));
            vTaskDelay(pdMS_TO_TICKS(dwell));
        }

        if (sweepRound % 5 == 0)
            LOG_WARN(DTAG, "Sweep ronde #%lu — gateway belum ditemukan. "
                     "Pastikan gateway sudah nyala.", sweepRound);
    }

    // Beacon ditemukan! Channel sudah di-set oleh _promiscuousRxCb.
    // Peer update di-handle oleh processPendingChannelSync() dari taskCSSender.
    LOG_INFO(DTAG, "Gateway ditemukan! ch=%d | %lu ronde sweep | "
             "promiscuous tetap ON untuk RSSI",
             s_channel, sweepRound);

    // Promiscuous tetap ON — dibutuhkan untuk RSSI monitoring
    // Task selesai, hapus diri sendiri
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
    if (!s_channelConfirmed)
    {
        // Fase sweep (sebelum esp_now_init): langsung set s_channel
        // esp_wifi_set_channel() aman dipanggil dari ISR
        static uint8_t s_candCh    = 0;
        static uint8_t s_candCount = 0;

        if (beaconCh != s_candCh) { s_candCh = beaconCh; s_candCount = 1; }
        else                        s_candCount++;

        if (s_candCount >= 2)  // 2 hits cukup — MAC matching sudah reliable
        {
            s_channel          = beaconCh;
            s_channelConfirmed = true;
            s_candCh           = 0;
            s_candCount        = 0;
            // esp_wifi_set_channel aman dari ISR
            esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

            // v4.0: Jika ESP-NOW sudah init (boot-anytime flow),
            // trigger peer channel update dari task context
            if (s_espnowReady)
            {
                s_pendingChannel    = beaconCh;
                s_needChannelUpdate = true;
            }
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