// File: firmware/lib/EspNowMesh/MeshRouting.cpp

// =============================================================================
// MeshRouting.cpp — Implementasi Routing Packet → MQTT
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "MeshRouting.h"
#include "Config.h"

static constexpr char TAG[] = "ROUTE";


// =============================================================================
// route() — Entry Point, Dispatch ke Handler yang Sesuai
//
// Pola dispatch ini (switch pada byte pertama) membuat penambahan
// packet type baru hanya butuh tambah satu case — tidak ada perubahan
// di tempat lain.
// =============================================================================
bool MeshRouting::route(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < 2)
    {
        LOG_WARN(TAG, "Packet terlalu pendek (%d bytes) — dibuang", raw.len);
        return false;
    }

    const uint8_t rawType = raw.data[0];

    // Dispatch berdasarkan tipe packet
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
// =============================================================================
bool MeshRouting::_routeCombined(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < sizeof(CombinedPacket))
    {
        LOG_WARN(TAG, "COMBINED terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CombinedPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const CombinedPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/combined", Mqtt::TOPIC_BASE, pkt->header.nodeId);

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
             pkt->ppg.valid       ? "true" : "false",
             pkt->edge.fingerOn   ? "true" : "false");

    LOG_DEBUG(TAG, "COMBINED node=%d ts=%lu",
              pkt->header.nodeId, (unsigned long)pkt->header.timestamp);
    return true;
}


// =============================================================================
// _routeHeartbeat() — Serialize HeartbeatPacket → JSON
// =============================================================================
bool MeshRouting::_routeHeartbeat(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < sizeof(HeartbeatPacket))
    {
        LOG_WARN(TAG, "HEARTBEAT terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(HeartbeatPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(raw.data);

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
//
// Dipakai untuk: ax, ay, az, gx, gy, gz (6 axis IMU).
// Nama axis ditentukan dari PacketType via _axisName().
// =============================================================================
bool MeshRouting::_routeCsAxis(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < sizeof(CS1AxisPacket))
    {
        LOG_WARN(TAG, "CS_AXIS terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CS1AxisPacket));
        return false;
    }

    const auto*  pkt      = reinterpret_cast<const CS1AxisPacket*>(raw.data);
    const char*  axisName = _axisName(raw.data[0]);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_%s",
             Mqtt::TOPIC_BASE, pkt->header.nodeId, axisName);

    // Bangun payload: header JSON + float array
    char* p   = out.payload;
    int   rem = sizeof(out.payload);

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
//
// Dipakai khusus untuk IR PPG yang punya metadata tambahan (HR, ppgValid).
// =============================================================================
bool MeshRouting::_routeCsIr(const RawPacket& raw, MqttMessage& out)
{
    if (raw.len < sizeof(CSPpgPacket))
    {
        LOG_WARN(TAG, "CS_IR terlalu pendek: %d < %d bytes",
                 raw.len, sizeof(CSPpgPacket));
        return false;
    }

    const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);

    snprintf(out.topic, sizeof(out.topic),
             "%s/node_%d/cs_ir", Mqtt::TOPIC_BASE, pkt->header.nodeId);

    char* p   = out.payload;
    int   rem = sizeof(out.payload);

    int w = snprintf(p, rem,
                     "{\"ts\":%lu,\"hr\":%d,\"ppg_valid\":%s,\"finger\":%s,\"y\":[",
                     (unsigned long)pkt->header.timestamp,
                     pkt->heartRate,
                     pkt->ppgValid      ? "true" : "false",
                     pkt->edge.fingerOn ? "true" : "false");
    p += w; rem -= w;

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
//
// Memisahkan logika ini ke helper mencegah duplikasi antara
// _routeCsAxis() dan _routeCsIr().
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