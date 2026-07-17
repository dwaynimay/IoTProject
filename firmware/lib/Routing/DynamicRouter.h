// File: firmware/lib/Routing/DynamicRouter.h

#pragma once
// =============================================================================
// DynamicRouter — Dynamic Routing Decision Engine for Multi-Hop Mesh
// =============================================================================
//
// Hardware  : none (pure logic / mathematics)
// Why this implementation:
//             Tracks relative signal strengths (RSSI) between self-to-gateway
//             and neighbor-to-gateway to dynamically select the best route
//             (Direct vs. Relayed) on a per-transmission-window basis.
//
// USAGE:
//   DynamicRouter router(NODE_ID);
//
//   // Upon receiving a gateway beacon:
//   router.updateSelfRssi(rssi_from_beacon);
//
//   // Upon receiving an RSSI report from a neighbor:
//   router.updateNeighborRssi(neighborId, neighbor_rssi_to_gateway);
//
//   // Before sending data:
//   RouteDecision dec = router.decide();
//   if (dec.isDirect) {
//       mesh.sendCsAxis(..., MacAddr::GATEWAY);
//   } else {
//       mesh.sendCsAxis(..., MacAddr::NEIGHBOR); // relay
//   }
//
// THREAD SAFETY:
//   The updateSelfRssi() function is called within the WiFi receive callback context,
//   while decide() is evaluated within the taskCSSender loop. Internal state updates
//   are protected via a FreeRTOS mutex (_mux).
// =============================================================================

#include <Arduino.h>
#include "MeshPackets.h"

// =============================================================================
// RouteDecision — Hasil keputusan routing
// =============================================================================
struct RouteDecision
{
    bool isDirect;         // true = kirim ke gateway langsung
    uint8_t nextHopNodeId; // jika !isDirect: node ID relay yang dipakai
    int8_t rssiSelf;       // RSSI self ke gateway saat ini (untuk logging)
    int8_t rssiNeighbor;   // RSSI neighbor ke gateway saat ini (untuk logging)
};

// =============================================================================
// DynamicRouter
// =============================================================================
class DynamicRouter
{
public:
    explicit DynamicRouter(uint8_t selfNodeId);

    // ── Update RSSI ───────────────────────────────────────────────────────────

    // Dipanggil saat terima BeaconPacket dari gateway.
    // rssi: nilai dari esp_now recv callback (dBm, negatif)
    void updateSelfRssi(int8_t rssi);

    // Dipanggil saat terima RssiReportPacket dari neighbor.
    void updateNeighborRssi(uint8_t neighborNodeId, int8_t rssiNeighborToGw);

    // ── Routing Decision ──────────────────────────────────────────────────────

    // Putuskan rute terbaik untuk window berikutnya.
    // Selalu return keputusan valid — fallback ke DIRECT jika RSSI stale.
    RouteDecision decide() const;

    // ── Status ────────────────────────────────────────────────────────────────

    bool isSelfRssiValid() const;
    bool isNeighborRssiValid() const;
    bool isDiscoveryDone() const;
    int8_t selfRssi() const { return _rssiSelf; }
    int8_t neighborRssi() const { return _rssiNeighbor; }
    uint8_t neighborNodeId() const { return _neighborNodeId; }

    // Print status routing ke Serial (untuk LOG_INFO)
    void printStatus() const;

private:
    uint8_t _selfNodeId;
    uint8_t _neighborNodeId; // ID primary neighbor (dari routing table)

    // === PHASE 3: N-Node Mesh Support ===
    // Daftar semua valid neighbors dari routing table
    // Routing engine bisa kemudian pick yang terbaik dari list ini
    uint8_t _validNeighbors[4];  // Array neighbor yang valid (dari MeshTopology)
    uint8_t _validNeighborCount; // Jumlah neighbors yang valid

    // Nilai RSSI terbaru (dBm)
    int8_t _rssiSelf = RoutingCfg::RSSI_UNKNOWN;
    int8_t _rssiNeighbor = RoutingCfg::RSSI_UNKNOWN;

    // Timestamp terakhir update (untuk deteksi stale)
    uint32_t _lastSelfUpdateMs = 0;
    uint32_t _lastNeighborUpdateMs = 0;

    // Waktu boot — untuk deteksi discovery phase
    uint32_t _bootMs;

    // Mutex untuk akses _rssiSelf dari dua task berbeda
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};
