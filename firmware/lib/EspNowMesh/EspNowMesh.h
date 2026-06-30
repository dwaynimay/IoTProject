// File: firmware/lib/EspNowMesh/EspNowMesh.h

#pragma once
// =============================================================================
// EspNowMesh.h — Transport Layer ESP-NOW dengan Multi-Hop Support
// =============================================================================
//
// PERBAIKAN v3.1:
//   [FIX-1] begin(false) di gateway tidak lagi memanggil esp_wifi_get_channel()
//           sebelum WiFi diinisialisasi. Channel sementara = 1.
//
//   [FIX-2] setGatewayChannel(ch) ditambahkan — dipanggil dari main.cpp
//           setelah g_mqtt.begin() untuk update semua peer ke channel WiFi asli.
//
//   [FIX-3] _promiscuousRxCb debounce diperbaiki (lihat EspNowMesh.cpp).
//
// URUTAN INISIALISASI GATEWAY (penting):
//   1. g_mesh.begin(false)         ← ESP-NOW init, channel sementara = 1
//   2. taskBeacon created          ← mulai broadcast di ch 1 (sementara)
//   3. g_mqtt.begin()              ← WiFi connect, dapat channel asli (mis. 11)
//   4. g_mesh.setGatewayChannel(ch)← update semua peer ke ch 11
//
// Sensor node tidak perlu setGatewayChannel() karena channel dideteksi
// via promiscuous callback dari beacon gateway.
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "MeshPackets.h"
#include "../../include/Config.h"


class EspNowMesh
{
public:
    EspNowMesh() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // senderMode = true  → sensor node (kirim ke gateway / neighbor)
    // senderMode = false → gateway node (terima dari semua node)
    // [FIX-1] Gateway: WiFi BELUM harus aktif saat begin() dipanggil.
    bool begin(bool senderMode);

    // ── Channel Management (Gateway Only) ─────────────────────────────────────

    // [FIX-2] Update channel ESP-NOW setelah WiFi gateway terkoneksi.
    // Panggil SEKALI dari main.cpp setelah g_mqtt.begin() berhasil.
    // Parameter: channel WiFi yang aktif (dari esp_wifi_get_channel atau WiFi.channel())
    void setGatewayChannel(uint8_t channel);

    // Proses deferred channel sync dari task context (bukan ISR).
    // Panggil di loop taskCSSender sebelum encode/send.
    // Return true jika channel baru di-apply.
    bool processPendingChannelSync();

    // ── Beacon API (Gateway Node Only) ────────────────────────────────────────

    // Broadcast beacon ke semua node untuk RSSI discovery.
    bool sendBeacon();

    // Broadcast time sync ke semua node (Gateway Node Only)
    bool sendTimeSync(uint32_t gatewayMillis);

    // ── RSSI Exchange API (Sensor Node) ───────────────────────────────────────

    bool sendRssiReport(uint8_t selfNodeId, int8_t rssiToGateway);

    // ── CS Send API (Sensor Node) ─────────────────────────────────────────────

    bool sendCsAxis(uint8_t pktType, uint8_t nodeId,
                    const float y[CS_M], float mean, bool fingerOn,
                    uint32_t timestamp,
                    const uint8_t* dstMac);

    bool sendCsPpg(uint8_t nodeId, const float yIr[CS_M],
                   float mean, int8_t heartRate, bool ppgValid,
                   float spo2, bool fingerOn,
                   uint32_t timestamp,
                   const uint8_t* dstMac);

    // ── Relay API ─────────────────────────────────────────────────────────────

    bool forwardRoutedCs(uint8_t relayNodeId,
                         uint8_t originalNodeId,
                         const uint8_t* innerData,
                         uint8_t innerLen);

    // ── Existing API ──────────────────────────────────────────────────────────

    bool sendCombined(const CombinedPacket& pkt);
    bool sendHeartbeat(uint8_t nodeId, uint32_t uptimeS);

    // ── Receive API ───────────────────────────────────────────────────────────

    // Ambil paket dari internal queue (non-blocking). Return true jika ada.
    bool readPacket(RawPacket& out);

    // Ambil metrik queue untuk keperluan monitoring (Gateway)
    void getQueueMetrics(UBaseType_t& used, UBaseType_t& free) const;

    // ── RSSI Measurement ──────────────────────────────────────────────────────

    int8_t getLastBeaconRssi() const { return _lastBeaconRssi; }
    void resetBeaconRssi() { _lastBeaconRssi = RoutingCfg::RSSI_UNKNOWN; }
    bool isChannelConfirmed() const;

    // ── Status ────────────────────────────────────────────────────────────────

    bool lastSendOk() const { return _lastSendOk; }

private:
    bool    _senderMode    = true;
    bool    _lastSendOk    = false;
    uint8_t _beaconSeqNum  = 0;

    volatile int8_t _lastBeaconRssi = RoutingCfg::RSSI_UNKNOWN;
    
    QueueHandle_t _rxQueue = nullptr;

    bool _send(const void* data, size_t len, const uint8_t* dstMac);
    bool _addPeer(const uint8_t* mac);
    bool _isPeerRegistered(const uint8_t* mac);

    static void _onDataSent(const uint8_t* mac, esp_now_send_status_t status);
    static void _onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
    static void _promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type);

    static EspNowMesh* _instance;
};