// File: firmware/lib/Routing/DynamicRouter.cpp

// =============================================================================
// DynamicRouter.cpp — Implementasi Dynamic Routing Decision Engine
// =============================================================================

#include "DynamicRouter.h"
#include "../../include/Config.h"
#include "../../include/config/tuning.h"

static constexpr char TAG[] = "ROUTER";


// =============================================================================
// Constructor
// =============================================================================
DynamicRouter::DynamicRouter(uint8_t selfNodeId)
    : _selfNodeId(selfNodeId)
    , _bootMs(millis())
    , _validNeighborCount(0)
{
    // Initialize valid neighbors array to zeros
    for (uint8_t idx = 0; idx < 4; idx++)
    {
        _validNeighbors[idx] = 0;
    }

    // === PHASE 3: Initialize from routing table ===
    // Baca neighbor(s) dari MeshTopology::nodeNeighbors untuk node ini
    // Dst: _validNeighbors[] berisi daftar neighbor yang bisa dipakai untuk relay
    
    // Validate node ID is within range
    if (selfNodeId >= MeshTopology::totalNodes)
    {
        LOG_ERROR(TAG, "Invalid selfNodeId: %d (max: %d)",
                  selfNodeId, MeshTopology::totalNodes - 1);
        _neighborNodeId = 0; // Fallback to gateway
        return;
    }

    // Copy neighbors dari routing table ke instance field — dengan bounds checking
    _validNeighborCount = 0;
    
    for (uint8_t i = 0; i < MeshTopology::maxNeighborsPerNode && i < 4; i++)
    {
        // Safely read from routing table with bounds checking
        if (i >= 1)  // maxNeighborsPerNode is 1 in current config
            break;
            
        uint8_t neighbor = MeshTopology::nodeNeighbors[selfNodeId][i];
        
        // Only add valid non-self references
        if (neighbor != selfNodeId && neighbor < MeshTopology::totalNodes)
        {
            _validNeighbors[_validNeighborCount] = neighbor;
            _validNeighborCount++;
        }
    }

    // Set primary neighbor (first valid relay, or gateway as default)
    if (_validNeighborCount > 0)
    {
        _neighborNodeId = _validNeighbors[0];
        LOG_INFO(TAG, "Router init | self=%d | neighbors=[", _selfNodeId);
        for (uint8_t i = 0; i < _validNeighborCount; i++)
        {
            LOG_INFO(TAG, "%d%s", _validNeighbors[i], i < _validNeighborCount - 1 ? "," : "]");
        }
        LOG_INFO(TAG, "threshold=%d dBm", RoutingCfg::RELAY_THRESHOLD_DBM);
    }
    else
    {
        // No relays available — only direct to gateway
        _neighborNodeId = 0; // GATEWAY_NODE_ID
        LOG_INFO(TAG, "Router init | self=%d | no relay neighbors, direct to gateway only",
                 _selfNodeId);
    }
}


// =============================================================================
// updateSelfRssi() — Dipanggil saat terima BeaconPacket dari gateway
//
// KONTEKS: WiFi task (recv callback) — gunakan critical section
// =============================================================================
void DynamicRouter::updateSelfRssi(int8_t rssi)
{
    taskENTER_CRITICAL(&_mux);
    _rssiSelf          = rssi;
    _lastSelfUpdateMs  = millis();
    taskEXIT_CRITICAL(&_mux);

    LOG_DEBUG(TAG, "RSSI self→gw updated: %d dBm", rssi);
}


// =============================================================================
// updateNeighborRssi() — Dipanggil saat terima RssiReportPacket
//
// KONTEKS: WiFi task (recv callback) — gunakan critical section
// === PHASE 3: Accept any valid neighbor from routing table ===
// =============================================================================
void DynamicRouter::updateNeighborRssi(uint8_t neighborNodeId, int8_t rssiNeighborToGw)
{
    // Check if this neighbor is in our valid neighbors list
    bool isValidNeighbor = false;
    for (uint8_t i = 0; i < _validNeighborCount; i++)
    {
        if (_validNeighbors[i] == neighborNodeId)
        {
            isValidNeighbor = true;
            break;
        }
    }

    if (!isValidNeighbor)
    {
        LOG_WARN(TAG, "RssiReport dari node %d bukan valid neighbor", neighborNodeId);
        return;
    }

    taskENTER_CRITICAL(&_mux);
    _rssiNeighbor          = rssiNeighborToGw;
    _lastNeighborUpdateMs  = millis();
    taskEXIT_CRITICAL(&_mux);

    LOG_DEBUG(TAG, "RSSI neighbor(%d)→gw updated: %d dBm",
              neighborNodeId, rssiNeighborToGw);
}


// =============================================================================
// decide() — Putuskan rute terbaik
//
// LOGIKA:
//   1. Jika masih dalam discovery phase → selalu DIRECT (data belum dikirim,
//      tapi jika dipaksa kirim, lebih aman langsung ke gateway)
//   2. Jika RSSI self stale → DIRECT (fallback aman)
//   3. Jika RSSI neighbor stale → DIRECT (tidak bisa andalkan relay)
//   4. Jika rssi_self >= rssi_neighbor - THRESHOLD → DIRECT
//      (self cukup dekat, tidak perlu relay)
//   5. Jika rssi_neighbor > rssi_self + THRESHOLD → RELAYED
//      (neighbor jauh lebih dekat ke gateway)
//
// THRESHOLD mencegah "flapping" — bolak-balik ganti rute saat RSSI
// hampir sama, yang bisa terjadi di lingkungan dengan multipath fading.
// =============================================================================
RouteDecision DynamicRouter::decide() const
{
    RouteDecision dec{};

    // Baca RSSI dengan proteksi (const method, tapi perlu baca atomic)
    // Pada ESP32 single-read int8_t bersifat atomic, tapi kita tetap
    // copy dulu untuk konsistensi
    const int8_t  rssiSelf     = _rssiSelf;
    const int8_t  rssiNeighbor = _rssiNeighbor;
    const uint32_t now         = millis();

    dec.rssiSelf     = rssiSelf;
    dec.rssiNeighbor = rssiNeighbor;

    // ── Cek discovery phase ───────────────────────────────────────────────────
    if (!isDiscoveryDone())
    {
        dec.isDirect      = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(10, LOG_INFO, TAG,
                    "Discovery phase aktif — kirim DIRECT (fallback)");
        return dec;
    }

    // ── Cek stale RSSI self ───────────────────────────────────────────────────
    const bool selfStale = !isSelfRssiValid();
    if (selfStale)
    {
        dec.isDirect      = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(20, LOG_WARN, TAG,
                    "RSSI self stale — fallback DIRECT");
        return dec;
    }

    // ── Cek stale RSSI neighbor ───────────────────────────────────────────────
    const bool neighborStale = !isNeighborRssiValid();
    if (neighborStale)
    {
        // Neighbor tidak terdengar — tidak bisa jadi relay
        dec.isDirect      = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;
        LOG_EVERY_N(20, LOG_WARN, TAG,
                    "RSSI neighbor stale — fallback DIRECT");
        return dec;
    }

    // ── Keputusan utama ───────────────────────────────────────────────────────
    // rssiNeighbor > rssiSelf berarti neighbor lebih DEKAT ke gateway
    // (nilai dBm yang lebih tinggi = sinyal lebih kuat = lebih dekat)
    //
    // Pakai relay HANYA jika neighbor secara signifikan lebih baik
    const int16_t diff = static_cast<int16_t>(rssiNeighbor)
                       - static_cast<int16_t>(rssiSelf);

    if (diff >= RoutingCfg::RELAY_THRESHOLD_DBM)
    {
        // Neighbor lebih dekat ke gateway → kirim ke neighbor untuk di-relay
        dec.isDirect      = false;
        dec.nextHopNodeId = _neighborNodeId;

        LOG_EVERY_N(5, LOG_INFO, TAG,
                    "Rute: RELAY via node %d | self=%d dBm neighbor=%d dBm diff=%d dBm",
                    _neighborNodeId, rssiSelf, rssiNeighbor, (int)diff);
    }
    else
    {
        // Self cukup dekat atau lebih dekat → kirim langsung
        dec.isDirect      = true;
        dec.nextHopNodeId = RoutingCfg::GATEWAY_NODE_ID;

        LOG_EVERY_N(5, LOG_INFO, TAG,
                    "Rute: DIRECT | self=%d dBm neighbor=%d dBm diff=%d dBm",
                    rssiSelf, rssiNeighbor, (int)diff);
    }

    return dec;
}


// =============================================================================
// Status helpers
// =============================================================================
bool DynamicRouter::isSelfRssiValid() const
{
    if (_rssiSelf == RoutingCfg::RSSI_UNKNOWN) return false;
    return (millis() - _lastSelfUpdateMs) < RoutingCfg::RSSI_STALE_MS;
}

bool DynamicRouter::isNeighborRssiValid() const
{
    if (_rssiNeighbor == RoutingCfg::RSSI_UNKNOWN) return false;
    return (millis() - _lastNeighborUpdateMs) < RoutingCfg::RSSI_STALE_MS;
}

bool DynamicRouter::isDiscoveryDone() const
{
    return (millis() - _bootMs) >= RoutingCfg::DISCOVERY_PHASE_MS;
}

void DynamicRouter::printStatus() const
{
    LOG_INFO(TAG,
             "─── Routing Status (Node %d) ───────────────────",
             _selfNodeId);
    LOG_INFO(TAG, "Discovery: %s | uptime=%lu s",
             isDiscoveryDone() ? "DONE" : "ACTIVE",
             (unsigned long)((millis() - _bootMs) / 1000));
    LOG_INFO(TAG, "RSSI self→gw   : %d dBm | valid=%s",
             _rssiSelf, isSelfRssiValid() ? "Y" : "N (stale)");
    LOG_INFO(TAG, "RSSI neighbor→gw: %d dBm | valid=%s",
             _rssiNeighbor, isNeighborRssiValid() ? "Y" : "N (stale)");

    RouteDecision dec = decide();
    LOG_INFO(TAG, "Keputusan saat ini: %s",
             dec.isDirect
             ? "DIRECT ke gateway"
             : "RELAY via neighbor");
    LOG_INFO(TAG, "────────────────────────────────────────────────");
}
