// File: firmware/lib/EspNowMesh/MeshRouting.h

#pragma once
// =============================================================================
// MeshRouting.h — Routing Packet ESP-NOW ke MQTT Topic
// =============================================================================
//
// PERUBAHAN v3.0 (Multi-Hop):
//   BARU:
//     _routeRoutedCs()  → unwrap RoutedCsPacket, proses inner payload
//     _routeRssiReport()→ update DynamicRouter dengan RSSI neighbor
//                         (hanya relevan di sensor node yang juga jadi relay)
//
// Catatan arsitektur:
//   Di GATEWAY: semua packet masuk (termasuk ROUTED_CS) diproses di sini.
//   Di SENSOR sebagai RELAY: _onDataRecv menerima CS packet dari neighbor
//   → langsung forwardRoutedCs() tanpa lewat MeshRouting (lebih cepat).
//   MeshRouting hanya jalan di GATEWAY.
// =============================================================================

#include "MeshPackets.h"
#include "../Routing/DynamicRouter.h"


enum class RouteResult
{
    PUBLISHED,      // siap publish ke MQTT
    ACCUMULATING,   // sedang akumulasi (normal)
    FORWARDED,      // paket di-relay (sensor node sebagai relay)
    DROPPED,        // paket invalid
};


// =============================================================================
// MeshRouting — Static Class
// =============================================================================
class MeshRouting
{
public:
    // Proses satu RawPacket.
    // router: pointer ke DynamicRouter sensor (nullptr di gateway node)
    static RouteResult route(const RawPacket& raw, MqttMessage& out,
                             DynamicRouter* router = nullptr);

private:
    // ── Router per Packet Type ────────────────────────────────────────────────
    static RouteResult _routeCombined  (const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeHeartbeat (const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeCsAxis    (const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeCsIr      (const RawPacket& raw, MqttMessage& out);

    // ── Multi-hop handlers ────────────────────────────────────────────────────

    // Unwrap RoutedCsPacket → proses inner payload seolah diterima langsung
    static RouteResult _routeRoutedCs  (const RawPacket& raw, MqttMessage& out);

    // Update DynamicRouter dengan RSSI dari neighbor
    // (hanya diproses di sensor node, bukan gateway)
    static RouteResult _routeRssiReport(const RawPacket& raw,
                                        DynamicRouter* router);

    // ── Helpers ───────────────────────────────────────────────────────────────
    static int _writeFloatArray(char* dst, int rem,
                                const float* arr, uint8_t len);
    static const char* _axisName(uint8_t rawType);

    // Buffer akumulasi IMU — index 0 = node 1, index 1 = node 2
    static ImuWindowBuffer _imuBuf[2];

    static inline uint8_t _nodeIdx(uint8_t nodeId)
    {
        return (nodeId >= 1 && nodeId <= 2) ? (nodeId - 1) : 0;
    }
};
