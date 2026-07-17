// File: firmware/lib/EspNowMesh/EspNowMesh.cpp
// =============================================================================
// EspNowMesh — Transport Layer ESP-NOW Implementation
// =============================================================================
//
// WiFi Channel Synchronization Flow:
//
// SENSOR NODE WORKFLOW:
//   1. WiFi.begin(SSID, PASSWORD)          <- Connect to the target AP shared with gateway
//   2. Wait for WL_CONNECTED (timeout 10s)
//   3. ch = WiFi.channel()                 <- Read the active AP channel
//   4. WiFi.disconnect() + WiFi.mode(OFF)  <- Disconnect from AP to free the radio
//   5. delay(200)                          <- Wait for radio hardware stabilizer
//   6. WiFi.mode(STA) + set channel        <- Prepare STA mode on retrieved channel
//   7. esp_now_init()                      <- Initialize ESP-NOW on the correct channel
//
// GATEWAY NODE WORKFLOW:
//   - Remains permanently connected to the WiFi AP (STA+AP mode).
//   - The ESP-NOW channel is locked by the active WiFi STA connection.
//   - Broadcast beacons are sent on this locked channel automatically.
//
// Technical Context:
//   - Eliminates timing dependency between sensor and gateway boot order.
//   - Avoids promiscuous mode packet sniffing and background channel sweeps.
//   - Requires WiFi credentials inside credentials.h to connect initially.
// =============================================================================

#include <esp_wifi.h>
#include "EspNowMesh.h"

static constexpr char TAG[] = "MESH";

static uint8_t  s_channel          = 1;
static bool     s_channelConfirmed = false;
static volatile bool s_espnowReady = false;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

extern QueueHandle_t g_mqttQueue;

EspNowMesh* EspNowMesh::_instance = nullptr;

// =============================================================================
// Synchronous send: semaphore + result flag
//
// _send() memanggil esp_now_send() lalu BLOK menunggu callback.
// Callback (_onDataSent) set s_sendResult lalu give semaphore.
// Timeout 50ms — cukup untuk worst-case MAC retry (ESP-NOW internal retry
// bisa sampai ~30ms sebelum menyerah dan NACK).
// =============================================================================
static SemaphoreHandle_t s_sendSem    = nullptr;
static volatile bool     s_sendResult = false;

// Mutex untuk serialisasi _send(): hanya satu unicast in-flight pada satu waktu.
// Mencegah race antara taskCSSender, taskRssiExchange, dan taskSensorReceiver
// yang sama-sama memanggil _send() (terutama di node yang jadi relay).
static SemaphoreHandle_t s_sendMutex  = nullptr;


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

    // Buat semaphore untuk synchronous send (sekali saja)
    if (!s_sendSem)
    {
        s_sendSem = xSemaphoreCreateBinary();
        if (!s_sendSem)
        {
            LOG_ERROR(TAG, "Gagal buat send semaphore!");
            return false;
        }
    }

    if (!s_sendMutex)
    {
        s_sendMutex = xSemaphoreCreateMutex();
        if (!s_sendMutex)
        {
            LOG_ERROR(TAG, "Gagal buat send mutex!");
            return false;
        }
    }

    if (!_rxQueue)
    {
        _rxQueue = xQueueCreate(20, sizeof(RawPacket));
        if (!_rxQueue)
        {
            LOG_ERROR(TAG, "Gagal buat rxQueue!");
            return false;
        }
    }

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
        WiFi.disconnect(true);      // arg1=wifioff: matikan radio WiFi.
                                    // JANGAN tambah arg2 (eraseap) — biarkan NVS utuh.
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
            if (!_addPeer(MacAddr::NODE_PPG)) return false;
            LOG_INFO(TAG, "Sensor %d: GATEWAY + NODE_PPG | ch=%d", NODE_ID, s_channel);
        #else
            if (!_addPeer(MacAddr::NODE_IMU)) return false;
            LOG_INFO(TAG, "Sensor %d: GATEWAY + NODE_IMU | ch=%d", NODE_ID, s_channel);
        #endif

        if (!_addPeer(BROADCAST_MAC)) return false;

        LOG_INFO(TAG, "Mode: SENSOR | MAC: %s | ch=%d",
                 WiFi.macAddress().c_str(), s_channel);
    }
    else
    {
        if (!_addPeer(MacAddr::NODE_IMU)) return false;
        if (!_addPeer(MacAddr::NODE_PPG)) return false;
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

void EspNowMesh::getQueueMetrics(UBaseType_t& used, UBaseType_t& free) const
{
    if (_rxQueue) {
        used = uxQueueMessagesWaiting(_rxQueue);
        free = uxQueueSpacesAvailable(_rxQueue);
    } else {
        used = 0;
        free = 0;
    }
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
// sendTimeSync() — (Gateway Only)
// =============================================================================
bool EspNowMesh::sendTimeSync(uint32_t gatewayMillis)
{
    TimeSyncPacket pkt{};
    pkt.header = { PacketType::TIME_SYNC, RoutingCfg::GATEWAY_NODE_ID, gatewayMillis };
    
    // Broadcast ke semua node
    const bool ok = _send(&pkt, sizeof(TimeSyncPacket), BROADCAST_MAC);
    LOG_INFO(TAG, "TimeSync sent: %lu ms | ok=%s", gatewayMillis, ok ? "Y" : "N");
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
        const uint8_t* neighborMac = MacAddr::NODE_PPG;
    #else
        const uint8_t* neighborMac = MacAddr::NODE_IMU;
    #endif
    return _send(&pkt, sizeof(RssiReportPacket), neighborMac);
}


// =============================================================================
// sendCsAxis() / sendCsPpg() / forwardRoutedCs() / sendCombined() / sendHeartbeat()
// (tidak ada perubahan dari v3.3)
// =============================================================================
bool EspNowMesh::sendCsAxis(uint8_t pktType, uint8_t nodeId,
                             const float y[CS_M], float mean, bool fingerOn,
                             uint32_t timestamp, const uint8_t* dstMac)
{
    CS1AxisPacket pkt{};
    pkt.header = { static_cast<PacketType>(pktType), nodeId, timestamp };
    memcpy(pkt.y, y, CS_M * sizeof(float));
    pkt.mean   = mean;
    pkt.edge   = { fingerOn, 0 };
    // vTaskDelay(1ms) dihapus v5.1 — eliminasi 7ms blocking per window
    return _send(&pkt, sizeof(CS1AxisPacket), dstMac);
}

bool EspNowMesh::sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                            float mean, int8_t heartRate, bool ppgValid, float spo2,
                            bool fingerOn, uint32_t timestamp,
                            const uint8_t* dstMac)
{
    CSPpgPacket pkt{};
    pkt.header    = { PacketType::CS_IR, nodeId, timestamp };
    memcpy(pkt.yIr, yIr, CS_M * sizeof(float));
    pkt.mean      = mean;
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

// ACK/NACK counters terpisah
static volatile uint32_t _ackCountGw  = 0;
static volatile uint32_t _nackCountGw = 0;
static volatile uint32_t _ackCountNode  = 0;
static volatile uint32_t _nackCountNode = 0;

bool EspNowMesh::_send(const void* data, size_t len, const uint8_t* dstMac)
{
    // Ambil mutex — serialisasi seluruh transaksi send+ack.
    // Timeout 100ms cukup longgar; jika gagal ambil, anggap send gagal.
    if (s_sendMutex &&
        xSemaphoreTake(s_sendMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        _lastSendOk = false;
        return false;
    }

    // Broadcast: tidak butuh channel management & tidak ada ACK
    const bool isBroadcast = (dstMac[0] == 0xFF && dstMac[1] == 0xFF &&
                               dstMac[2] == 0xFF && dstMac[3] == 0xFF &&
                               dstMac[4] == 0xFF && dstMac[5] == 0xFF);

    if (!isBroadcast && s_lastSentChannel != s_channel)
    {
        _updateAllPeerChannels(s_channel);
        s_lastSentChannel = s_channel;
        LOG_WARN(TAG, "Peer channel diupdate ke ch=%d", s_channel);
    }

    // Drain semaphore sebelum send (buang sisa dari callback sebelumnya)
    xSemaphoreTake(s_sendSem, 0);

    const esp_err_t err = esp_now_send(
        dstMac, reinterpret_cast<const uint8_t*>(data), len);
    if (err != ESP_OK)
    {
        LOG_EVERY_N(10, LOG_WARN, TAG, "esp_now_send err: 0x%X", err);
        _lastSendOk = false;
        if (s_sendMutex) xSemaphoreGive(s_sendMutex);
        return false;
    }

    // Broadcast tidak punya ACK — anggap sukses
    if (isBroadcast)
    {
        _lastSendOk = true;
        if (s_sendMutex) xSemaphoreGive(s_sendMutex);
        return true;
    }

    // Unicast: tunggu callback ACK/NACK (max 50ms)
    // ESP-NOW internal MAC retry butuh ~10-30ms sebelum menyerah.
    if (xSemaphoreTake(s_sendSem, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        _lastSendOk = s_sendResult;
        const bool result = s_sendResult;
        if (s_sendMutex) xSemaphoreGive(s_sendMutex);
        return result;
    }

    // Timeout — anggap NACK
    _lastSendOk = false;
    bool isGw = (memcmp(dstMac, MacAddr::GATEWAY, 6) == 0);
    if (isGw) _nackCountGw++; else _nackCountNode++;
    
    LOG_EVERY_N(10, LOG_WARN, TAG, "send timeout (50ms) to %s — dianggap NACK", isGw ? "GW" : "NODE");
    if (s_sendMutex) xSemaphoreGive(s_sendMutex);
    return false;
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

void EspNowMesh::_onDataSent(const uint8_t* mac, esp_now_send_status_t status)
{
    bool isGw = (memcmp(mac, MacAddr::GATEWAY, 6) == 0);

    if (status == ESP_NOW_SEND_SUCCESS)
    {
        if (isGw) _ackCountGw++; else _ackCountNode++;
        s_sendResult = true;
    }
    else
    {
        if (isGw)
        {
            _nackCountGw++;
            s_sendResult = false;
        }
        else
        {
            _nackCountNode++;
            s_sendResult = false;
        }
    }

    // Sinyal ke _send() yang sedang menunggu
    if (s_sendSem)
    {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_sendSem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void EspNowMesh::_onDataRecv(const uint8_t* mac, const uint8_t* data, int len)
{
    if (len < 1) return;
    const uint8_t pktType = data[0];

    if (pktType == static_cast<uint8_t>(PacketType::BEACON))
    {
        // Beacon hanya digunakan untuk RSSI tracking di promiscuous mode,
        // abaikan payload data-nya agar tidak memenuhi queue
        return;
    }

    if (_instance && _instance->_rxQueue)
    {
        RawPacket raw{};
        raw.len = static_cast<uint8_t>(len <= 250 ? len : 250);
        memcpy(raw.data,   data, raw.len);
        memcpy(raw.srcMac, mac,  6);

        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(_instance->_rxQueue, &raw, &woken);
        if (woken == pdTRUE) portYIELD_FROM_ISR();
    }
}

// =============================================================================
// readPacket() — Ambil paket dari internal queue
// =============================================================================
bool EspNowMesh::readPacket(RawPacket& out)
{
    if (!_rxQueue) return false;
    return xQueueReceive(_rxQueue, &out, 0) == pdTRUE;
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

void EspNowMesh::_promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    if (!_instance) return;

    const wifi_promiscuous_pkt_t* ppkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);

    const uint8_t*  payload    = ppkt->payload;
    const uint16_t  payloadLen = ppkt->rx_ctrl.sig_len;

    // ESP-NOW action frame minimum size ~35 bytes.
    // Beacon packet kita total = 42 bytes (termasuk MAC header).
    if (payloadLen < 35) return;

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