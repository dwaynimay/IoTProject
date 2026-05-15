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


// =============================================================================
// route() — Entry Point, Dispatch ke Handler yang Sesuai
// =============================================================================
bool MeshRouting::route(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < 2)
    {
        LOG_WARN(TAG, "Packet terlalu pendek (%d bytes) — dibuang", raw.len);
        return false;
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
            return false;
    }
}


// =============================================================================
// _routeCombined() — Serialize CombinedPacket → JSON
// FIXED: semua field lama diganti ke camelCase
// =============================================================================
bool MeshRouting::_routeCombined(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CombinedPacket)))
    {
        LOG_WARN(TAG, "COMBINED terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CombinedPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const CombinedPacket*>(raw.data);

    // FIXED: node_id → nodeId
    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/combined", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    // FIXED: accel_x → accelX, gyro_x → gyroX, ir_raw → irRaw,
    //        red_raw → redRaw, heart_rate → heartRate, finger_on → fingerOn
    snprintf(out.payload, sizeof(out.payload),
             "{"
             "\"ts\":%lu,"
             "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
             "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
             "\"ir\":%lu,\"red\":%lu,"
             "\"hr\":%d,\"spo2\":%.1f,\"ppg_valid\":%s,"
             "\"finger\":%s"
             "}",
             (unsigned long)pkt->header.timestamp,
             pkt->imu.accelX, pkt->imu.accelY, pkt->imu.accelZ,
             pkt->imu.gyroX,  pkt->imu.gyroY,  pkt->imu.gyroZ,
             (unsigned long)pkt->ppg.irRaw,
             (unsigned long)pkt->ppg.redRaw,
             pkt->ppg.heartRate,
             pkt->ppg.spo2,
             pkt->ppg.valid      ? "true" : "false",
             pkt->edge.fingerOn  ? "true" : "false");

    LOG_DEBUG(TAG, "COMBINED node=%d ts=%lu",
              pkt->header.nodeId, (unsigned long)pkt->header.timestamp);
    return true;
}


// =============================================================================
// _routeHeartbeat() — Serialize HeartbeatPacket → JSON
// FIXED: node_id → nodeId (uptimeS sudah camelCase di MeshPackets.h baru)
// =============================================================================
bool MeshRouting::_routeHeartbeat(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(HeartbeatPacket)))
    {
        LOG_WARN(TAG, "HEARTBEAT terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(HeartbeatPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(raw.data);

    // FIXED: node_id → nodeId
    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    snprintf(out.payload, sizeof(out.payload),
             "{\"ts\":%lu,\"uptime\":%lu}",
             (unsigned long)pkt->header.timestamp,
             (unsigned long)pkt->uptimeS);

    LOG_DEBUG(TAG, "HEARTBEAT node=%d uptime=%lu s",
              pkt->header.nodeId, (unsigned long)pkt->uptimeS);
    return true;
}


// =============================================================================
// _routeCsAxis() — Serialize CS1AxisPacket → JSON
// FIXED: node_id → nodeId, finger_on → fingerOn
// =============================================================================
bool MeshRouting::_routeCsAxis(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CS1AxisPacket)))
    {
        LOG_WARN(TAG, "CS_AXIS terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CS1AxisPacket));
        return false;
    }

    const auto*  pkt      = reinterpret_cast<const CS1AxisPacket*>(raw.data);
    const char*  axisName = _axisName(raw.data[0]);

    // FIXED: node_id → nodeId
    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_%s",
             Mqtt::TOPIC_BASE, pkt->header.nodeId, axisName);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);

    // FIXED: finger_on → fingerOn
    int w = snprintf(p, rem,
                     "{\"ts\":%lu,\"finger\":%s,\"y\":[",
                     (unsigned long)pkt->header.timestamp,
                     pkt->edge.fingerOn ? "true" : "false");
    p += w; rem -= w;

    w = _writeFloatArray(p, rem, pkt->y, CS_M);
    p += w; rem -= w;

    snprintf(p, rem, "]}");

    LOG_DEBUG(TAG, "CS_%s node=%d ts=%lu finger=%s",
              axisName, pkt->header.nodeId,
              (unsigned long)pkt->header.timestamp,
              pkt->edge.fingerOn ? "Y" : "N");
    return true;
}


// =============================================================================
// _routeCsIr() — Serialize CSPpgPacket → JSON
// FIXED: node_id → nodeId, y_ir → yIr, heart_rate → heartRate,
//        ppg_valid → ppgValid, finger_on → fingerOn
// =============================================================================
bool MeshRouting::_routeCsIr(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < static_cast<int>(sizeof(CSPpgPacket)))
    {
        LOG_WARN(TAG, "CS_IR terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CSPpgPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);

    // FIXED: node_id → nodeId
    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_ir", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);

    // FIXED: heart_rate → heartRate, ppg_valid → ppgValid, finger_on → fingerOn
    int w = snprintf(p, rem,
                     "{\"ts\":%lu,\"hr\":%d,\"ppg_valid\":%s,\"finger\":%s,\"y\":[",
                     (unsigned long)pkt->header.timestamp,
                     pkt->heartRate,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    p += w; rem -= w;

    // FIXED: y_ir → yIr
    w = _writeFloatArray(p, rem, pkt->yIr, CS_M);
    p += w; rem -= w;

    snprintf(p, rem, "]}");

    LOG_DEBUG(TAG, "CS_IR node=%d HR=%d finger=%s",
              pkt->header.nodeId, pkt->heartRate,
              pkt->edge.fingerOn ? "Y" : "N");
    return true;
}


// =============================================================================
// _writeFloatArray() — Tulis float[] sebagai JSON array
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


// =============================================================================
// _axisName() — Nama Axis dari PacketType Raw Byte
// =============================================================================
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