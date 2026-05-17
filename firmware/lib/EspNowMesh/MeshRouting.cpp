// File: firmware/lib/EspNowMesh/MeshRouting.cpp

// =============================================================================
// MeshRouting.cpp — Implementasi Routing v3.0 (Multi-Hop)
// =============================================================================
//
// PERUBAHAN v3.0:
//   - route() terima parameter router (DynamicRouter*) opsional
//   - Tambah case ROUTED_CS  → _routeRoutedCs()
//   - Tambah case RSSI_REPORT → _routeRssiReport()
//   - Tambah case BEACON     → diabaikan di gateway (tidak perlu diproses)
//
// ALUR ROUTED_CS di GATEWAY:
//   1. Terima RoutedCsPacket dari relay node
//   2. _routeRoutedCs() baca header: originalNodeId, innerLen
//   3. Buat RawPacket sementara dari inner payload
//   4. Dispatch ke _routeCsAxis() atau _routeCsIr() dengan originalNodeId
//   5. Hasil JSON sama persis seolah diterima langsung dari original node
//   6. Tambahkan field "relayed_by" di JSON untuk audit trail
// =============================================================================

#include "MeshRouting.h"
#include "../../include/Config.h"

static constexpr char TAG[] = "ROUTE";

ImuWindowBuffer MeshRouting::_imuBuf[2] = {};


// =============================================================================
// route() — Dispatch utama
// =============================================================================
RouteResult MeshRouting::route(const RawPacket& raw, MqttMessage& out,
                               DynamicRouter* router)
{
    if (raw.len < 2)
    {
        LOG_WARN(TAG, "Packet terlalu pendek (%d bytes) — dibuang", raw.len);
        return RouteResult::DROPPED;
    }

    const uint8_t rawType = raw.data[0];

    switch (rawType)
    {
        case static_cast<uint8_t>(PacketType::BEACON):
            // Beacon diproses di EspNowMesh via promiscuous callback.
            // Di gateway tidak perlu diproses di sini — abaikan saja.
            return RouteResult::DROPPED;

        case static_cast<uint8_t>(PacketType::RSSI_REPORT):
            // Hanya relevan di sensor node (update DynamicRouter).
            // Di gateway tidak ada router → abaikan.
            return _routeRssiReport(raw, router);

        case static_cast<uint8_t>(PacketType::ROUTED_CS):
            // Paket CS yang di-relay oleh neighbor node → unwrap
            return _routeRoutedCs(raw, out);

        case static_cast<uint8_t>(PacketType::COMBINED_DATA):
            return _routeCombined(raw, out);

        case static_cast<uint8_t>(PacketType::HEARTBEAT):
            return _routeHeartbeat(raw, out);

        case PKT_CS_AX:
        case PKT_CS_AY:
        case PKT_CS_AZ:
        case PKT_CS_GX:
        case PKT_CS_GY:
        case PKT_CS_GZ:
            return _routeCsAxis(raw, out);

        case PKT_CS_IR:
            return _routeCsIr(raw, out);

        default:
            LOG_WARN(TAG, "Tipe packet tidak dikenal: 0x%02X", rawType);
            return RouteResult::DROPPED;
    }
}


// =============================================================================
// _routeRssiReport() — Update DynamicRouter dengan RSSI dari neighbor
//
// Hanya diproses jika router != nullptr (sensor node).
// Di gateway, router = nullptr → return DROPPED (tidak diproses lebih lanjut).
// =============================================================================
RouteResult MeshRouting::_routeRssiReport(const RawPacket& raw,
                                           DynamicRouter* router)
{
    if (!router)
    {
        // Di gateway — tidak perlu proses RSSI report
        return RouteResult::DROPPED;
    }

    if (raw.len < static_cast<int>(sizeof(RssiReportPacket)))
    {
        LOG_WARN(TAG, "RSSI_REPORT terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(RssiReportPacket));
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const RssiReportPacket*>(raw.data);

    router->updateNeighborRssi(pkt->header.nodeId, pkt->rssiToGateway);

    LOG_DEBUG(TAG, "RSSI_REPORT dari node %d: %d dBm ke gateway",
              pkt->header.nodeId, pkt->rssiToGateway);

    // Tidak publish ke MQTT — ini pesan internal routing
    return RouteResult::ACCUMULATING;
}


// =============================================================================
// _routeRoutedCs() — Unwrap RoutedCsPacket dan proses inner payload
//
// Gateway menerima paket ini dari relay node.
// Inner payload adalah CS1AxisPacket atau CSPpgPacket asli dari original node.
// Kita unwrap lalu dispatch ke handler yang sesuai.
// Field "relayed_by" ditambahkan di JSON untuk audit trail.
// =============================================================================
RouteResult MeshRouting::_routeRoutedCs(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(RoutedCsHeader)))
    {
        LOG_WARN(TAG, "ROUTED_CS terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(RoutedCsHeader));
        return RouteResult::DROPPED;
    }

    const auto* routedHdr = reinterpret_cast<const RoutedCsHeader*>(raw.data);

    if (routedHdr->innerLen == 0 ||
        routedHdr->innerLen > sizeof(RoutedCsPacket::inner))
    {
        LOG_WARN(TAG, "ROUTED_CS innerLen tidak valid: %d", routedHdr->innerLen);
        return RouteResult::DROPPED;
    }

    LOG_DEBUG(TAG, "ROUTED_CS: original_node=%d relay=%d innerLen=%d latency=%lums",
              routedHdr->originalNodeId,
              routedHdr->relayNodeId,
              routedHdr->innerLen,
              (unsigned long)(millis() - routedHdr->relayTimestamp));

    // Buat RawPacket sementara dari inner payload
    // sehingga bisa di-dispatch ke handler yang sudah ada
    RawPacket innerRaw{};
    innerRaw.len = routedHdr->innerLen;
    memcpy(innerRaw.data,
           raw.data + sizeof(RoutedCsHeader),
           routedHdr->innerLen);
    memcpy(innerRaw.srcMac, raw.srcMac, 6);

    // Dispatch ke handler yang sesuai berdasarkan tipe inner packet
    const uint8_t innerType = innerRaw.data[0];
    RouteResult result = RouteResult::DROPPED;

    if (innerType == PKT_CS_IR)
    {
        result = _routeCsIr(innerRaw, out);
    }
    else if (innerType >= PKT_CS_AX && innerType <= PKT_CS_GZ)
    {
        result = _routeCsAxis(innerRaw, out);
    }
    else
    {
        LOG_WARN(TAG, "ROUTED_CS inner type tidak dikenal: 0x%02X", innerType);
        return RouteResult::DROPPED;
    }

    // Jika berhasil di-route (PUBLISHED), tambahkan info relay ke payload
    // Format: insert "relayed_by":N sebelum closing brace "}"
    if (result == RouteResult::PUBLISHED)
    {
        // Cari posisi "}" terakhir di payload
        const size_t payloadLen = strlen(out.payload);
        if (payloadLen > 0 && out.payload[payloadLen - 1] == '}')
        {
            // Sisipkan field relay sebelum "}"
            char relayInfo[32];
            snprintf(relayInfo, sizeof(relayInfo),
                     ",\"relayed_by\":%d}", routedHdr->relayNodeId);

            // Overwrite "}" terakhir dengan relayInfo
            const size_t remaining = sizeof(out.payload) - payloadLen;
            if (remaining > strlen(relayInfo))
            {
                // Ganti "}" terakhir
                out.payload[payloadLen - 1] = '\0';
                strncat(out.payload, relayInfo, remaining - 1);
            }
        }

        LOG_DEBUG(TAG, "ROUTED_CS processed | original=%d relay=%d → %s",
                  routedHdr->originalNodeId,
                  routedHdr->relayNodeId,
                  out.topic);
    }

    return result;
}


// =============================================================================
// _routeCombined() — tidak berubah dari v2
// =============================================================================
RouteResult MeshRouting::_routeCombined(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CombinedPacket)))
    {
        LOG_WARN(TAG, "COMBINED terlalu pendek: %d bytes", raw.len);
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const CombinedPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/combined", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    if (pkt->ppg.spo2 > 0.0f)
    {
        snprintf(out.payload, sizeof(out.payload),
                 "{\"ts\":%lu,"
                 "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                 "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                 "\"ir\":%lu,\"red\":%lu,"
                 "\"hr\":%d,\"spo2\":%.1f,\"ppg_valid\":%s,"
                 "\"finger\":%s}",
                 (unsigned long)pkt->header.timestamp,
                 pkt->imu.accelX, pkt->imu.accelY, pkt->imu.accelZ,
                 pkt->imu.gyroX,  pkt->imu.gyroY,  pkt->imu.gyroZ,
                 (unsigned long)pkt->ppg.irRaw,
                 (unsigned long)pkt->ppg.redRaw,
                 pkt->ppg.heartRate, pkt->ppg.spo2,
                 pkt->ppg.valid     ? "true" : "false",
                 pkt->edge.fingerOn ? "true" : "false");
    }
    else
    {
        snprintf(out.payload, sizeof(out.payload),
                 "{\"ts\":%lu,"
                 "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                 "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                 "\"ir\":%lu,\"red\":%lu,"
                 "\"hr\":%d,\"spo2\":null,\"ppg_valid\":%s,"
                 "\"finger\":%s}",
                 (unsigned long)pkt->header.timestamp,
                 pkt->imu.accelX, pkt->imu.accelY, pkt->imu.accelZ,
                 pkt->imu.gyroX,  pkt->imu.gyroY,  pkt->imu.gyroZ,
                 (unsigned long)pkt->ppg.irRaw,
                 (unsigned long)pkt->ppg.redRaw,
                 pkt->ppg.heartRate,
                 pkt->ppg.valid     ? "true" : "false",
                 pkt->edge.fingerOn ? "true" : "false");
    }

    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeHeartbeat() — tidak berubah dari v2
// =============================================================================
RouteResult MeshRouting::_routeHeartbeat(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(HeartbeatPacket)))
        return RouteResult::DROPPED;

    const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    snprintf(out.payload, sizeof(out.payload),
             "{\"ts\":%lu,\"uptime\":%lu,\"rssi_abs\":%d}",
             (unsigned long)pkt->header.timestamp,
             (unsigned long)pkt->uptimeS,
             pkt->rssi);

    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeCsAxis() — tidak berubah dari v2
// =============================================================================
RouteResult MeshRouting::_routeCsAxis(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CS1AxisPacket)))
    {
        LOG_WARN(TAG, "CS_AXIS terlalu pendek: %d bytes", raw.len);
        return RouteResult::DROPPED;
    }

    const auto*    pkt    = reinterpret_cast<const CS1AxisPacket*>(raw.data);
    const uint8_t  axIdx  = raw.data[0] - PKT_CS_AX;
    const uint8_t  bufIdx = _nodeIdx(pkt->header.nodeId);

    ImuWindowBuffer& buf = _imuBuf[bufIdx];

    // Stale detection
    if (buf.receivedMask != 0 &&
        buf.receivedMask != IMU_ALL_RECEIVED &&
        (millis() - buf.lastUpdateMs) > 2000)
    {
        LOG_WARN(TAG, "Node %d: IMU buffer timeout (mask=0x%02X) — reset",
                 buf.nodeId, buf.receivedMask);
        buf.receivedMask = 0;
    }

    // Deteksi window baru
    if (buf.receivedMask != 0 && pkt->header.timestamp != buf.timestamp)
    {
        uint32_t diff = (pkt->header.timestamp > buf.timestamp)
                        ? pkt->header.timestamp - buf.timestamp
                        : buf.timestamp - pkt->header.timestamp;

        if (diff < 100)
            buf.timestamp = pkt->header.timestamp;
        else
        {
            LOG_WARN(TAG, "Node %d: window baru ts=%lu (diff=%lums) — reset",
                     pkt->header.nodeId,
                     (unsigned long)pkt->header.timestamp,
                     (unsigned long)diff);
            buf.receivedMask = 0;
        }
    }

    // Simpan axis
    float* dsts[] = {buf.ax, buf.ay, buf.az, buf.gx, buf.gy, buf.gz};
    memcpy(dsts[axIdx], pkt->y, CS_M * sizeof(float));
    buf.receivedMask |= (1u << axIdx);
    buf.timestamp     = pkt->header.timestamp;
    buf.fingerOn      = pkt->edge.fingerOn;
    buf.nodeId        = pkt->header.nodeId;
    buf.lastUpdateMs  = millis();

    if (buf.receivedMask != IMU_ALL_RECEIVED)
    {
        LOG_DEBUG(TAG, "IMU buf node=%d mask=0x%02X (%d axis lagi)",
                  buf.nodeId, buf.receivedMask,
                  6 - __builtin_popcount(buf.receivedMask));
        return RouteResult::ACCUMULATING;
    }

    // Semua 6 axis terkumpul
    buf.receivedMask = 0;

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_imu", Mqtt::TOPIC_BASE, buf.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);
    int   w;

    w = snprintf(p, rem, "{\"ts\":%lu,\"finger\":%s",
                 (unsigned long)buf.timestamp,
                 buf.fingerOn ? "true" : "false");
    p += w; rem -= w;

    const char* names[] = {"ax","ay","az","gx","gy","gz"};
    float*      arrs[]  = {buf.ax,buf.ay,buf.az,buf.gx,buf.gy,buf.gz};

    for (uint8_t i = 0; i < 6; i++)
    {
        w = snprintf(p, rem, ",\"%s\":[", names[i]);
        p += w; rem -= w;
        w = _writeFloatArray(p, rem, arrs[i], CS_M);
        p += w; rem -= w;
        w = snprintf(p, rem, "]");
        p += w; rem -= w;
    }
    snprintf(p, rem, "}");

    // Guard: jika payload mendekati batas, log warning
    const size_t used = strlen(out.payload);
    if (used > 1700)
    {
        LOG_WARN(TAG, "cs_imu payload besar: %d bytes — pertimbangkan kurangi presisi", used);
    }

    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeCsIr() — tidak berubah dari v2
// =============================================================================
RouteResult MeshRouting::_routeCsIr(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CSPpgPacket)))
    {
        LOG_WARN(TAG, "CS_IR terlalu pendek: %d bytes", raw.len);
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_ppg", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);
    int   w;

    if (pkt->spo2 > 0.0f)
    {
        w = snprintf(p, rem,
                     "{\"ts\":%lu,\"hr\":%d,\"spo2\":%.1f,"
                     "\"ppg_valid\":%s,\"finger\":%s,\"ir\":[",
                     (unsigned long)pkt->header.timestamp,
                     pkt->heartRate, pkt->spo2,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    }
    else
    {
        w = snprintf(p, rem,
                     "{\"ts\":%lu,\"hr\":%d,\"spo2\":null,"
                     "\"ppg_valid\":%s,\"finger\":%s,\"ir\":[",
                     (unsigned long)pkt->header.timestamp,
                     pkt->heartRate,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    }
    p += w; rem -= w;

    w = _writeFloatArray(p, rem, pkt->yIr, CS_M);
    p += w; rem -= w;
    snprintf(p, rem, "]}");

    return RouteResult::PUBLISHED;
}


// =============================================================================
// Helpers
// =============================================================================
int MeshRouting::_writeFloatArray(char* dst, int rem,
                                  const float* arr, uint8_t len)
{
    int total = 0;
    for (uint8_t i = 0; i < len && rem > 15; i++)
    {
        const int w = snprintf(dst, rem, i ? ",%.4f" : "%.4f", arr[i]);
        dst += w; rem -= w; total += w;
    }
    return total;
}

const char* MeshRouting::_axisName(uint8_t rawType)
{
    switch (rawType)
    {
        case PKT_CS_AX: return "ax";
        case PKT_CS_AY: return "ay";
        case PKT_CS_AZ: return "az";
        case PKT_CS_GX: return "gx";
        case PKT_CS_GY: return "gy";
        case PKT_CS_GZ: return "gz";
        default:        return "unknown";
    }
}
