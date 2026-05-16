// File: firmware/lib/Routing/DynamicRouter.cpp

// =============================================================================
// DynamicRouter.cpp — Implementasi Dynamic Routing Decision Engine
// =============================================================================

#include "DynamicRouter.h"
#include "../../include/Config.h"

static constexpr char TAG[] = "ROUTER";


// =============================================================================
// Constructor
// =============================================================================
DynamicRouter::DynamicRouter(uint8_t selfNodeId)
    : _selfNodeId(selfNodeId)
    , _bootMs(millis())
{
    // Neighbor node ID: jika self=1 maka neighbor=2, dan sebaliknya
    // Untuk sistem 2 node ini cukup. Jika ada lebih banyak node,
    // ganti dengan routing table yang lebih kompleks.
    _neighborNodeId = (selfNodeId == 1) ? 2 : 1;

    LOG_INFO(TAG, "Router init | self=%d neighbor=%d | threshold=%d dBm",
             _selfNodeId, _neighborNodeId,
             RoutingCfg::RELAY_THRESHOLD_DBM);
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
// =============================================================================
void DynamicRouter::updateNeighborRssi(uint8_t neighborNodeId, int8_t rssiNeighborToGw)
{
    if (neighborNodeId != _neighborNodeId)
    {
        LOG_WARN(TAG, "RssiReport dari node %d tidak dikenal (expected %d)",
                 neighborNodeId, _neighborNodeId);
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
