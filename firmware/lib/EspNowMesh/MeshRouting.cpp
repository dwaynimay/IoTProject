// File: firmware/lib/EspNowMesh/MeshRouting.cpp

// =============================================================================
// MeshRouting.cpp — Implementasi Routing Packet → MQTT
// =============================================================================
// PERUBAHAN v2 (refactor):
//   Semua field access diupdate ke camelCase sesuai MeshPackets.h baru:
//     pkt->header.node_id  → pkt->header.nodeId
//     pkt->imu.accel_x     → pkt->imu.accelX
//     pkt->ppg.ir_raw      → pkt->ppg.irRaw
//     pkt->ppg.heart_rate  → pkt->ppg.heartRate
//     pkt->ppg.spo2        → pkt->ppg.spo2  (tidak berubah)
//     pkt->ppg.valid       → pkt->ppg.valid  (tidak berubah)
//     pkt->edge.finger_on  → pkt->edge.fingerOn
//     pkt->uptimeS         → pkt->uptimeS  (sudah benar di HeartbeatPacket)
//     pkt->yIr             → pkt->yIr
//     pkt->ppgValid        → pkt->ppgValid
//     pkt->heartRate       → pkt->heartRate (CSPpgPacket)
// =============================================================================

#include "MeshRouting.h"
#include "../../include/Config.h"

static constexpr char TAG[] = "ROUTE";

ImuWindowBuffer MeshRouting::_imuBuf[2] = {};


// =============================================================================
// route()
// =============================================================================
RouteResult MeshRouting::route(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < 2)
    {
        LOG_WARN(TAG, "Packet terlalu pendek (%d bytes) — dibuang", raw.len);
        return RouteResult::DROPPED;
    }

    const uint8_t rawType = raw.data[0];

    switch (rawType)
    {
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
// _routeCombined()
// =============================================================================
RouteResult MeshRouting::_routeCombined(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CombinedPacket)))
    {
        LOG_WARN(TAG, "COMBINED terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CombinedPacket));
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const CombinedPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/combined", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    // spo2: tampilkan nilai nyata jika valid, null jika tidak
    if (pkt->ppg.spo2 > 0.0f)
    {
        snprintf(out.payload, sizeof(out.payload),
                 "{"
                 "\"ts\":%llu,"
                 "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                 "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                 "\"ir\":%lu,\"red\":%lu,"
                 "\"hr\":%d,\"spo2\":%.1f,\"ppg_valid\":%s,"
                 "\"finger\":%s"
                 "}",
                 (unsigned long long)pkt->header.timestamp,
                 pkt->imu.accelX, pkt->imu.accelY, pkt->imu.accelZ,
                 pkt->imu.gyroX,  pkt->imu.gyroY,  pkt->imu.gyroZ,
                 (unsigned long)pkt->ppg.irRaw,
                 (unsigned long)pkt->ppg.redRaw,
                 pkt->ppg.heartRate,
                 pkt->ppg.spo2,
                 pkt->ppg.valid     ? "true" : "false",
                 pkt->edge.fingerOn ? "true" : "false");
    }
    else
    {
        snprintf(out.payload, sizeof(out.payload),
                 "{"
                 "\"ts\":%llu,"
                 "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                 "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                 "\"ir\":%lu,\"red\":%lu,"
                 "\"hr\":%d,\"spo2\":null,\"ppg_valid\":%s,"
                 "\"finger\":%s"
                 "}",
                 (unsigned long long)pkt->header.timestamp,
                 pkt->imu.accelX, pkt->imu.accelY, pkt->imu.accelZ,
                 pkt->imu.gyroX,  pkt->imu.gyroY,  pkt->imu.gyroZ,
                 (unsigned long)pkt->ppg.irRaw,
                 (unsigned long)pkt->ppg.redRaw,
                 pkt->ppg.heartRate,
                 pkt->ppg.valid     ? "true" : "false",
                 pkt->edge.fingerOn ? "true" : "false");
    }

    LOG_DEBUG(TAG, "COMBINED node=%d ts=%llu spo2=%.1f%%",
              pkt->header.nodeId,
              (unsigned long long)pkt->header.timestamp,
              pkt->ppg.spo2);
    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeHeartbeat()
// =============================================================================
RouteResult MeshRouting::_routeHeartbeat(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(HeartbeatPacket)))
    {
        LOG_WARN(TAG, "HEARTBEAT terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(HeartbeatPacket));
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    snprintf(out.payload, sizeof(out.payload),
             "{\"ts\":%llu,\"uptime\":%lu}",
             (unsigned long long)pkt->header.timestamp,
             (unsigned long)pkt->uptimeS);

    LOG_DEBUG(TAG, "HEARTBEAT node=%d uptime=%lu s",
              pkt->header.nodeId, (unsigned long)pkt->uptimeS);
    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeCsAxis()
// =============================================================================
RouteResult MeshRouting::_routeCsAxis(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CS1AxisPacket)))
    {
        LOG_WARN(TAG, "CS_AXIS terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CS1AxisPacket));
        return RouteResult::DROPPED;
    }

    const auto*    pkt    = reinterpret_cast<const CS1AxisPacket*>(raw.data);
    const uint8_t  axIdx  = raw.data[0] - PKT_CS_AX;
    const uint8_t  bufIdx = _nodeIdx(pkt->header.nodeId);

    ImuWindowBuffer& buf = _imuBuf[bufIdx];

    // Cek stale — reset jika buffer tidak lengkap dalam 2 detik
    if (buf.receivedMask != 0 &&
        buf.receivedMask != IMU_ALL_RECEIVED &&
        (millis() - buf.lastUpdateMs) > 2000)
    {
        LOG_WARN(TAG, "Node %d: IMU buffer timeout (mask=0x%02X) — reset",
                 buf.nodeId, buf.receivedMask);
        buf.receivedMask = 0;
    }

    // Deteksi window baru
    if (buf.receivedMask != 0 &&
        pkt->header.timestamp != buf.timestamp)
    {
        uint32_t diff = (pkt->header.timestamp > buf.timestamp)
                        ? pkt->header.timestamp - buf.timestamp
                        : buf.timestamp - pkt->header.timestamp;

        if (diff < 100)
        {
            buf.timestamp = pkt->header.timestamp;
        }
        else
        {
            LOG_WARN(TAG, "Node %d: window baru ts=%llu (diff=%lums) — reset buffer",
                     pkt->header.nodeId,
                     (unsigned long long)pkt->header.timestamp,
                     (unsigned long)diff);
            buf.receivedMask = 0;
        }
    }

    // Simpan axis ke buffer
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

    // Semua 6 axis terkumpul — format JSON
    buf.receivedMask = 0;

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_imu", Mqtt::TOPIC_BASE, buf.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);
    int   w;

    w = snprintf(p, rem, "{\"ts\":%llu,\"finger\":%s",
                 (unsigned long long)buf.timestamp,
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

    LOG_DEBUG(TAG, "cs_imu node=%d ts=%llu — publish",
              buf.nodeId, (unsigned long long)buf.timestamp);
    return RouteResult::PUBLISHED;
}


// =============================================================================
// _routeCsIr() — v2.1: tambah spo2 di JSON
//
// Format JSON cs_ppg:
//   { "ts": 12345, "hr": 72, "spo2": 98.5, "ppg_valid": true,
//     "finger": true, "ir": [...] }
//
// Jika spo2 tidak valid (pkt->spo2 == 0.0), field ditulis sebagai null
// agar server Python bisa membedakan "tidak tersedia" vs "nilai 0%".
// =============================================================================
RouteResult MeshRouting::_routeCsIr(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CSPpgPacket)))
    {
        LOG_WARN(TAG, "CS_IR terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CSPpgPacket));
        return RouteResult::DROPPED;
    }

    const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_ppg", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);
    int   w;

    // Header JSON dengan HR dan SpO2
    if (pkt->spo2 > 0.0f)
    {
        w = snprintf(p, rem,
                     "{\"ts\":%llu,\"hr\":%d,\"spo2\":%.1f,"
                     "\"ppg_valid\":%s,\"finger\":%s,\"ir\":[",
                     (unsigned long long)pkt->header.timestamp,
                     pkt->heartRate,
                     pkt->spo2,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    }
    else
    {
        w = snprintf(p, rem,
                     "{\"ts\":%llu,\"hr\":%d,\"spo2\":null,"
                     "\"ppg_valid\":%s,\"finger\":%s,\"ir\":[",
                     (unsigned long long)pkt->header.timestamp,
                     pkt->heartRate,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    }
    p += w; rem -= w;

    w = _writeFloatArray(p, rem, pkt->yIr, CS_M);
    p += w; rem -= w;
    snprintf(p, rem, "]}");

    LOG_DEBUG(TAG, "cs_ppg node=%d HR=%d SpO2=%.1f%% finger=%s",
              pkt->header.nodeId,
              pkt->heartRate,
              pkt->spo2,
              pkt->edge.fingerOn ? "Y" : "N");
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
        const int w = snprintf(dst, rem, i ? ",%.5f" : "%.5f", arr[i]);
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