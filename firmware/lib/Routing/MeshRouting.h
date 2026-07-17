// File: firmware/lib/Routing/MeshRouting.h

#pragma once
// =============================================================================
// MeshRouting — Router Engine mapping ESP-NOW Packets to MQTT Topics
// =============================================================================
//
// Key Responsibilities:
//   - _routeRoutedCs()  : Unwraps a RoutedCsPacket and processes the inner payload.
//   - _routeRssiReport(): Updates the DynamicRouter with the neighbor's RSSI data
//                         (relevant for sensor nodes acting as relays).
//
// Architectural Flow:
//   - Gateway Node : All incoming packets (including ROUTED_CS) are fully processed
//                    and routed to MQTT here.
//   - Relay Sensor : When _onDataRecv receives a packet from a neighbor, it directly
//                    calls forwardRoutedCs() to bypass the routing parser for speed.
//                    Thus, MeshRouting parser only executes on the Gateway Node.
// =============================================================================

#include "../EspNowMesh/MeshPackets.h"
#include "DynamicRouter.h"


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

    // Buffer akumulasi IMU — index 0 = node 1, index 1 = node 2
    static ImuWindowBuffer _imuBuf[2];

    static inline uint8_t _nodeIdx(uint8_t nodeId)
    {
        return (nodeId >= 1 && nodeId <= 2) ? (nodeId - 1) : 0;
    }
};
