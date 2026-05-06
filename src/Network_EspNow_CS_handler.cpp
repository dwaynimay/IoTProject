// =============================================================================
// Network_EspNow_CS_handler_deploy.cpp
//
// Tambahkan handler ini ke onDataRecv() di Network_EspNow.cpp
// SETELAH handler HEARTBEAT, sebelum default case.
//
// Handler menerima 7 paket CS1AxisPacket per window dan format ke JSON.
// Setiap paket langsung di-push ke g_mqttQueue (tidak perlu tunggu semua 7).
// Server Python yang bertanggung jawab sync 7 topic per window.
// =============================================================================

// ── Tambahkan include ini di atas Network_EspNow.cpp ──────────────────────
// #include "DataModels_CS.h"

// ── Helper serialisasi float array ke JSON — tambahkan sebagai static func ──
/*
static void serializeFloatArray(char* buf, int bufLen,
                                const char* topic_base, uint8_t nodeId,
                                const char* axis_name,
                                const float* y, uint8_t m,
                                uint32_t ts, bool finger,
                                QueueHandle_t queue) {
    MqttMessage msg{};
    snprintf(msg.topic, sizeof(msg.topic),
             "%s/node_%d/cs_%s", topic_base, nodeId, axis_name);

    // Format: {"ts":..., "finger":..., "y":[f0,f1,...,fM-1]}
    char* p = msg.payload;
    int   rem = sizeof(msg.payload);
    int   w;

    w   = snprintf(p, rem, "{\"ts\":%lu,\"finger\":%s,\"y\":[",
                   ts, finger ? "true" : "false");
    p  += w; rem -= w;

    for (uint8_t i = 0; i < m && rem > 15; i++) {
        w = snprintf(p, rem, i ? ",%.5f" : "%.5f", y[i]);
        p += w; rem -= w;
    }
    snprintf(p, rem, "]}");

    xQueueSendFromISR(queue, &msg, nullptr);
}
*/

// ── Handler di dalam onDataRecv() ─────────────────────────────────────────
/*
    // ── CS sinyal tunggal (ax / ay / az / gx / gy / gz / ir) ────────────
    uint8_t raw_type = static_cast<uint8_t>(type);
    if (raw_type >= PKT_CS_AX && raw_type <= PKT_CS_IR) {

        const char* axis_names[] = {"ax","ay","az","gx","gy","gz","ir"};
        uint8_t axis_idx = raw_type - PKT_CS_AX;  // 0..6

        if (raw_type == PKT_CS_IR) {
            // CSPpgPacket — ada metadata tambahan
            if (len < static_cast<int>(sizeof(CSPpgPacket))) return;
            const auto* pkt = reinterpret_cast<const CSPpgPacket*>(data);

            MqttMessage msg{};
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/cs_ir", Mqtt::TOPIC_BASE, nodeId);

            char* p = msg.payload;
            int rem = sizeof(msg.payload);
            int w = snprintf(p, rem,
                     "{\"ts\":%lu,\"hr\":%d,\"ppg_valid\":%s,\"finger\":%s,\"y\":[",
                     pkt->header.timestamp, pkt->heart_rate,
                     pkt->ppg_valid ? "true" : "false",
                     pkt->edge.finger_on ? "true" : "false");
            p += w; rem -= w;
            for (uint8_t i = 0; i < CS_M && rem > 15; i++) {
                w = snprintf(p, rem, i ? ",%.5f" : "%.5f", pkt->y_ir[i]);
                p += w; rem -= w;
            }
            snprintf(p, rem, "]}");
            xQueueSendFromISR(g_mqttQueue, &msg, nullptr);

        } else {
            // CS1AxisPacket — IMU axis
            if (len < static_cast<int>(sizeof(CS1AxisPacket))) return;
            const auto* pkt = reinterpret_cast<const CS1AxisPacket*>(data);

            MqttMessage msg{};
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/cs_%s",
                     Mqtt::TOPIC_BASE, nodeId, axis_names[axis_idx]);

            char* p = msg.payload;
            int rem = sizeof(msg.payload);
            int w = snprintf(p, rem,
                     "{\"ts\":%lu,\"finger\":%s,\"y\":[",
                     pkt->header.timestamp,
                     pkt->edge.finger_on ? "true" : "false");
            p += w; rem -= w;
            for (uint8_t i = 0; i < CS_M && rem > 15; i++) {
                w = snprintf(p, rem, i ? ",%.5f" : "%.5f", pkt->y[i]);
                p += w; rem -= w;
            }
            snprintf(p, rem, "]}");
            xQueueSendFromISR(g_mqttQueue, &msg, nullptr);
        }
        return;
    }
*/