// File: firmware/lib/Routing/DynamicRouter.cpp
// =============================================================================
// DynamicRouter — Dynamic Routing Decision Engine Implementation
// =============================================================================
//
// Implementation Details & Notes:
//   - Constructor: Neighbor initialization iterates up to maxNeighborsPerNode
//     to properly record valid peers.
//   - updateNeighborRssi: Supported for 2-node environments where the gateway (ID=0)
//     might act as a direct neighbor, preventing redundant "invalid neighbor" log
//     events.
//   - Direct-Only Fallback: If no valid neighbors are present (_validNeighborCount == 0),
//     routing skips evaluation and falls back immediately to direct transmission mode.
// =============================================================================

#include "DynamicRouter.h"
#include "../../include/Config.h"
#include "../../include/config/tuning.h"

static constexpr char TAG[] = "ROUTER";

// =============================================================================
// Constructor
// =============================================================================
DynamicRouter::DynamicRouter(uint8_t selfNodeId)
    : _selfNodeId(selfNodeId), _neighborNodeId(RoutingCfg::GATEWAY_NODE_ID), _validNeighborCount(0), _bootMs(millis())
{
    // Initialize valid neighbors array
    for (uint8_t i = 0; i < 4; i++)
        _validNeighbors[i] = 0;

    // Validasi selfNodeId
    if (selfNodeId >= MeshTopology::totalNodes)
    {
        LOG_ERROR(TAG, "Invalid selfNodeId: %d (max: %d)",
                  selfNodeId, MeshTopology::totalNodes - 1);
        _neighborNodeId = RoutingCfg::GATEWAY_NODE_ID;
        _validNeighborCount = 0;
        return;
    }

    // [FIX-4] Baca neighbor dari routing table dengan bounds check yang benar
    _validNeighborCount = 0;

    for (uint8_t i = 0;
         i < MeshTopology::maxNeighborsPerNode && _validNeighborCount < 4;
         i++)
    {
        const uint8_t neighbor = MeshTopology::nodeNeighbors[selfNodeId][i];

        // Lewati jika entry adalah self atau di luar range
        if (neighbor == selfNodeId || neighbor >= MeshTopology::totalNodes)
            continue;

        // Untuk sistem 2-node: neighbor = 0 (gateway) berarti tidak ada relay
        // node sebenarnya. Kita simpan saja untuk tracking, tapi relay tidak
        // akan dipakai (decide() akan selalu DIRECT jika satu-satunya neighbor
        // adalah gateway dengan RSSI yang sama atau lebih buruk).
        _validNeighbors[_validNeighborCount] = neighbor;
        _validNeighborCount++;
    }

    // Set primary neighbor
    if (_validNeighborCount > 0 && _validNeighbors[0] != RoutingCfg::GATEWAY_NODE_ID)
    {
        _neighborNodeId = _validNeighbors[0];
        LOG_INFO(TAG, "Router init | self=%d | relay neighbor=%d | threshold=%d dBm",
                 _selfNodeId, _neighborNodeId, RoutingCfg::RELAY_THRESHOLD_DBM);
    }
    else
    {
        // Tidak ada relay neighbor yang valid (sistem 2-node atau konfigurasi star)
        _neighborNodeId = RoutingCfg::GATEWAY_NODE_ID;
        _validNeighborCount = 0;
        LOG_INFO(TAG,
                 "Router init | self=%d | no relay (direct-only mode) | threshold=%d dBm",
                 _selfNodeId, RoutingCfg::RELAY_THRESHOLD_DBM);
    }
}

// =============================================================================
// updateSelfRssi()
// =============================================================================
void DynamicRouter::updateSelfRssi(int8_t rssi)
{
    taskENTER_CRITICAL(&_mux);
    if (RoutingOverride::ENABLE_MANUAL_RSSI) {
        _rssiSelf = RoutingOverride::MANUAL_SELF_RSSI;
    } else {
        _rssiSelf = rssi;
    }
    _lastSelfUpdateMs = millis();
    taskEXIT_CRITICAL(&_mux);

    LOG_DEBUG(TAG, "RSSI self→gw: %d dBm", _rssiSelf);
}

// =============================================================================
// updateNeighborRssi()
//
// [FIX-5] Untuk sistem 2-node (maxNeighborsPerNode=1, semua entry=0):
//   _validNeighborCount == 0 → tidak ada relay → abaikan semua RSSI report
//   dengan log DEBUG (bukan WARN) agar tidak spam.
// =============================================================================
void DynamicRouter::updateNeighborRssi(uint8_t neighborNodeId,
                                       int8_t rssiNeighborToGw)
{
    // Jika tidak ada relay neighbor yang dikonfigurasi — abaikan
    if (_validNeighborCount == 0)
    {
        LOG_DEBUG(TAG,
                  "updateNeighborRssi: direct-only mode, skip (dari node %d)",
                  neighborNodeId);
        return;
    }

    // Cek apakah neighbor ini ada dalam daftar relay yang valid
    bool isValidRelay = false;
    for (uint8_t i = 0; i < _validNeighborCount; i++)
    {
        if (_validNeighbors[i] == neighborNodeId)
        {
            isValidRelay = true;
            break;
        }
    }

    if (!isValidRelay)
    {
        LOG_WARN(TAG, "RssiReport dari node %d bukan relay neighbor",
                 neighborNodeId);
        return;
    }

    taskENTER_CRITICAL(&_mux);
    if (RoutingOverride::ENABLE_MANUAL_RSSI) {
        _rssiNeighbor = RoutingOverride::MANUAL_NEIGHBOR_RSSI;
    } else {
        _rssiNeighbor = rssiNeighborToGw;
    }
    _lastNeighborUpdateMs = millis();
    taskEXIT_CRITICAL(&_mux);

    LOG_DEBUG(TAG, "RSSI neighbor(%d)→gw: %d dBm",
              neighborNodeId, _rssiNeighbor);
}

// =============================================================================
// decide() — Putuskan rute terbaik
// =============================================================================
RouteDecision DynamicRouter::decide() const
{
    RouteDecision dec{};

    const int8_t rssiSelf = _rssiSelf;
    const int8_t rssiNeighbor = _rssiNeighbor;

    dec.rssiSelf = rssiSelf;
    dec.rssiNeighbor = rssiNeighbor;

    // ── Selalu DIRECT jika tidak ada relay neighbor ───────────────────────────
    if (_validNeighborCount == 0)
    {
        dec.isDirect = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        return dec;
    }

    // ── Cek discovery phase ───────────────────────────────────────────────────
    if (!isDiscoveryDone())
    {
        dec.isDirect = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(10, LOG_INFO, TAG, "Discovery aktif — DIRECT (fallback)");
        return dec;
    }

    // ── Cek stale RSSI self ───────────────────────────────────────────────────
    if (!isSelfRssiValid())
    {
        dec.isDirect = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(20, LOG_WARN, TAG, "RSSI self stale — fallback DIRECT");
        return dec;
    }

    // ── Cek stale RSSI neighbor ───────────────────────────────────────────────
    if (!isNeighborRssiValid())
    {
        dec.isDirect = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(20, LOG_WARN, TAG, "RSSI neighbor stale — fallback DIRECT");
        return dec;
    }

    // ── Keputusan: DIRECT vs RELAY ────────────────────────────────────────────
    const int16_t diff = static_cast<int16_t>(rssiNeighbor) - static_cast<int16_t>(rssiSelf);

    if (diff >= RoutingCfg::RELAY_THRESHOLD_DBM)
    {
        dec.isDirect = false;
        dec.nextHopNodeId = _neighborNodeId;

        LOG_EVERY_N(5, LOG_INFO, TAG,
                    "Rute: RELAY via node %d | self=%d dBm neighbor=%d dBm diff=%d",
                    _neighborNodeId, rssiSelf, rssiNeighbor, (int)diff);
    }
    else
    {
        dec.isDirect = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;

        LOG_EVERY_N(5, LOG_INFO, TAG,
                    "Rute: DIRECT | self=%d dBm neighbor=%d dBm diff=%d",
                    rssiSelf, rssiNeighbor, (int)diff);
    }

    return dec;
}

// =============================================================================
// Status helpers
// =============================================================================
bool DynamicRouter::isSelfRssiValid() const
{
    if (_rssiSelf == RoutingCfg::RSSI_UNKNOWN)
        return false;
    return (millis() - _lastSelfUpdateMs) < RoutingCfg::RSSI_STALE_MS;
}

bool DynamicRouter::isNeighborRssiValid() const
{
    if (_rssiNeighbor == RoutingCfg::RSSI_UNKNOWN)
        return false;
    return (millis() - _lastNeighborUpdateMs) < RoutingCfg::RSSI_STALE_MS;
}

bool DynamicRouter::isDiscoveryDone() const
{
    return (millis() - _bootMs) >= RoutingCfg::DISCOVERY_PHASE_MS;
}

void DynamicRouter::printStatus() const
{
    LOG_INFO(TAG, "─── Routing Status (Node %d) ───────────────────",
             _selfNodeId);
    LOG_INFO(TAG, "Discovery: %s | uptime=%lu s",
             isDiscoveryDone() ? "DONE" : "ACTIVE",
             (unsigned long)((millis() - _bootMs) / 1000));
    LOG_INFO(TAG, "Relay mode: %s | neighbor count=%d",
             _validNeighborCount > 0 ? "ENABLED" : "DISABLED (direct-only)",
             _validNeighborCount);
    LOG_INFO(TAG, "RSSI self→gw    : %d dBm | valid=%s",
             _rssiSelf, isSelfRssiValid() ? "Y" : "N (stale)");
    LOG_INFO(TAG, "RSSI neighbor→gw: %d dBm | valid=%s",
             _rssiNeighbor, isNeighborRssiValid() ? "Y" : "N (stale)");

    const RouteDecision dec = decide();
    LOG_INFO(TAG, "Keputusan: %s",
             dec.isDirect ? "DIRECT ke gateway" : "RELAY via neighbor");
    LOG_INFO(TAG, "────────────────────────────────────────────────");
}