// File: firmware/lib/EspNowMesh/EspNowMesh.h

#pragma once
// =============================================================================
// EspNowMesh.h — Transport Layer ESP-NOW dengan Multi-Hop Support
// =============================================================================
//
// PERUBAHAN v3.0 (Multi-Hop Dynamic Routing):
//   BARU:
//     sendBeacon()      → Gateway kirim beacon periodik
//     sendRssiReport()  → Node kirim info RSSI ke neighbor
//     forwardRoutedCs() → Node relay forward paket ke gateway
//     getRssiFromLastRecv() → Baca RSSI dari paket terakhir diterima
//
//   ARSITEKTUR (Sensor Node):
//     1. Boot → discovery phase (DISCOVERY_PHASE_MS)
//        - Terima beacon → ukur RSSI self ke gateway
//        - Kirim/terima RssiReport dengan neighbor
//     2. Setiap window:
//        - DynamicRouter::decide() → DIRECT atau RELAYED
//        - DIRECT  : sendCsAxis/sendCsPpg → GATEWAY
//        - RELAYED : sendCsAxis/sendCsPpg → NEIGHBOR
//                    (neighbor akan forwardRoutedCs → GATEWAY)
//
//   ARSITEKTUR (Gateway Node):
//     1. taskBeacon → sendBeacon() setiap BEACON_INTERVAL_MS
//     2. _onDataRecv → terima RoutedCsPacket → unwrap → push ke rawQueue
//        (RoutedCsPacket ditangani transparan, routing info disimpan di header)
//
// CATATAN RSSI:
//   ESP-NOW tidak expose RSSI di recv callback secara langsung.
//   Kita pakai workaround: promiscuous mode filter + wifi_pkt_rx_ctrl_t
//   untuk baca RSSI dari beacon packet.
//   Lihat _promiscuousRxCb() di EspNowMesh.cpp.
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "MeshPackets.h"
#include "../../include/Config.h"
#include "../Routing/DynamicRouter.h"

extern QueueHandle_t g_rawQueue;
extern QueueHandle_t g_mqttQueue;

// Pointer ke router sensor node — nullptr di gateway
extern DynamicRouter* g_routerPtr;


class EspNowMesh
{
public:
    EspNowMesh() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // senderMode = true  → sensor node (kirim ke gateway / neighbor)
    // senderMode = false → gateway node (terima dari semua node)
    bool begin(bool senderMode);

    // ── Beacon API (Gateway Node Only) ────────────────────────────────────────

    // Broadcast beacon ke semua node untuk RSSI discovery.
    // Panggil setiap BEACON_INTERVAL_MS dari taskBeacon.
    bool sendBeacon();

    // ── RSSI Exchange API (Sensor Node) ───────────────────────────────────────

    // Kirim RssiReportPacket ke neighbor node.
    // rssiToGateway: hasil pengukuran RSSI self ke gateway dari beacon.
    bool sendRssiReport(uint8_t selfNodeId, int8_t rssiToGateway);

    // ── CS Send API (Sensor Node) ─────────────────────────────────────────────
    // Sama seperti v2, tapi sekarang dst bisa GATEWAY atau NEIGHBOR MAC.
    // Caller (taskCSSender) yang menentukan tujuan berdasarkan RouteDecision.

    bool sendCsAxis(uint8_t pktType, uint8_t nodeId,
                    const float y[CS_M], bool fingerOn,
                    uint32_t timestamp,
                    const uint8_t* dstMac);   // ← NEWпараметер dst MAC

    bool sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                   int8_t heartRate, bool ppgValid,
                   float spo2, bool fingerOn,
                   uint32_t timestamp,
                   const uint8_t* dstMac);    // ← NEW parameter dst MAC

    // ── Relay API (Sensor Node sebagai Relay) ─────────────────────────────────

    // Bungkus inner payload dalam RoutedCsPacket lalu kirim ke gateway.
    // Dipanggil saat node menerima CS packet dari neighbor (bukan dari gateway).
    // innerData : pointer ke CS1AxisPacket atau CSPpgPacket yang diterima
    // innerLen  : panjang inner payload (sizeof(CS1AxisPacket) atau sizeof(CSPpgPacket))
    bool forwardRoutedCs(uint8_t relayNodeId,
                         uint8_t originalNodeId,
                         const uint8_t* innerData,
                         uint8_t innerLen);

    // ── Existing API (tidak berubah) ──────────────────────────────────────────

    bool sendCombined(const CombinedPacket& pkt);
    bool sendHeartbeat(uint8_t nodeId, uint32_t uptimeS);

    // ── RSSI Measurement ──────────────────────────────────────────────────────

    // Baca RSSI dari beacon terakhir yang diterima.
    // Return RSSI_UNKNOWN jika belum pernah terima beacon.
    int8_t getLastBeaconRssi() const { return _lastBeaconRssi; }

    // ── Status ────────────────────────────────────────────────────────────────

    bool lastSendOk() const { return _lastSendOk; }

private:
    bool    _senderMode    = true;
    bool    _lastSendOk    = false;
    uint8_t _beaconSeqNum  = 0;         // counter untuk gateway beacon

    // RSSI dari beacon terakhir (diupdate di promiscuous callback)
    volatile int8_t _lastBeaconRssi = RoutingCfg::RSSI_UNKNOWN;

    bool _send(const void* data, size_t len, const uint8_t* dstMac);
    bool _addPeer(const uint8_t* mac);
    bool _isPeerRegistered(const uint8_t* mac);

    // Callback ESP-NOW standar
    static void _onDataSent(const uint8_t* mac, esp_now_send_status_t status);
    static void _onDataRecv(const uint8_t* mac, const uint8_t* data, int len);

    // Promiscuous callback untuk baca RSSI dari beacon
    // ESP-NOW recv callback tidak expose RSSI — workaround via promiscuous mode
    static void _promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type);

    static EspNowMesh* _instance;
};
