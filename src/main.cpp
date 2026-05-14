// =============================================================================
// main.cpp — Entry Point & Orkestrasi FreeRTOS Task
//
// SENSOR NODE (ROLE_SENSOR): tidak berubah dari sebelumnya.
//
// GATEWAY NODE (ROLE_GATEWAY):
//   Init: WiFi konek dulu → esp_now_init → register callback
//
//   [Item #5 ISR Offload] Pipeline baru:
//
//   [ESP-NOW onDataRecv ISR]   →   [taskSerialize]   →   [taskMqttPublish]
//     memcpy raw bytes              format JSON            mqtt.publish()
//     ~1µs (aman)                   (Core 1)               (Core 0)
//         ↓                              ↓
//      g_rawQueue                   g_mqttQueue
//
//   taskSerialize (BARU):
//     - Ambil RawPacket dari g_rawQueue
//     - Routing berdasarkan PacketType (COMBINED, HEARTBEAT, CS_*)
//     - Serialisasi JSON (snprintf, loop CS_M)
//     - Push MqttMessage ke g_mqttQueue
//     - Semua logika yang sebelumnya di ISR onDataRecv ada di sini
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "DataModels.h"
#include "DataModels_CS.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "Network_EspNow.h"
#include "Network_Mqtt.h"
#include "Watchdog.h"

extern void taskCSSender(void *param);

// ===========================================================================
// SENSOR NODE
// ===========================================================================
#if NODE_ROLE == ROLE_SENSOR

static SensorMPU g_imu;
static SensorPPG g_ppg;
static NetworkEspNow g_espnow;

static SemaphoreHandle_t g_wire0Mutex = nullptr; // Wire  — MAX30102 (pin 18/19)
static SemaphoreHandle_t g_wire1Mutex = nullptr; // Wire1 — MPU6050  (pin 21/22)

portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;

ImuSample g_latestImu{};
PpgSample g_latestPpg{};

// ---------------------------------------------------------------------------
// taskReadPPG — Core 1, prioritas tertinggi
// ---------------------------------------------------------------------------
static void taskReadPPG(void *param)
{
    g_watchdog.registerTask();

    for (;;)
    {
        g_watchdog.feed();

        if (xSemaphoreTake(g_wire0Mutex, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            g_ppg.update();
            xSemaphoreGive(g_wire0Mutex);
        }
        else
        {
            Serial.println("[PPG] WARN: mutex timeout 200ms");
        }

        PpgSample snap{};
        g_ppg.read(snap);

        taskENTER_CRITICAL(&g_stateMux);
        g_latestPpg = snap;
        taskEXIT_CRITICAL(&g_stateMux);

        static uint32_t iter = 0;
        if (++iter % 100 == 0)
            g_watchdog.checkTaskStack("PPG");

        taskYIELD();
    }
}

// ---------------------------------------------------------------------------
// taskReadIMU — Core 1
// ---------------------------------------------------------------------------
static void taskReadIMU(void *param)
{
    g_watchdog.registerTask();
    uint32_t lastRead = 0;
    uint8_t failCount = 0;

    for (;;)
    {
        g_watchdog.feed();

        uint32_t now = millis();
        if (now - lastRead >= Timing::IMU_SAMPLE_MS)
        {
            ImuSample snap{};
            bool ok = false;

            if (xSemaphoreTake(g_wire1Mutex, portMAX_DELAY) == pdTRUE)
            {
                ok = g_imu.read(snap);
                xSemaphoreGive(g_wire1Mutex);
            }

            if (ok)
            {
                failCount = 0;
                taskENTER_CRITICAL(&g_stateMux);
                g_latestImu = snap;
                taskEXIT_CRITICAL(&g_stateMux);
            }
            else
            {
                failCount++;
                if (failCount > 50)
                    g_watchdog.triggerRestart("IMU read fail 50x");
            }
            lastRead = now;
        }

        static uint32_t iter = 0;
        if (++iter % 200 == 0)
            g_watchdog.checkTaskStack("IMU");

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// taskSendEspNow — Core 0
// ---------------------------------------------------------------------------
static void taskSendEspNow(void *param)
{
    g_watchdog.registerTask();

    TickType_t xLastWake    = xTaskGetTickCount();
    uint32_t lastHeartbeat  = 0;
    uint32_t lastDebugLog   = 0;
    uint32_t sentCount      = 0;
    uint32_t nackCount      = 0;

    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("[ESPNOW] taskSendEspNow dimulai...");

    for (;;)
    {
        g_watchdog.feed();

        ImuSample imuSnap{};
        PpgSample ppgSnap{};
        taskENTER_CRITICAL(&g_stateMux);
        imuSnap = g_latestImu;
        ppgSnap = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        EdgeResult edge{};
        edge.finger_on = (ppgSnap.ir_raw >= EdgeConfig::IR_FINGER_THRESHOLD);
        edge.reserved  = 0;

        bool shouldSend = true;
        if (EdgeConfig::ENABLE_FINGER_GATE && !edge.finger_on)
            shouldSend = false;

        if (millis() - lastDebugLog >= 2000)
        {
            Serial.printf("[DBG] IR=%lu | finger=%s | gate=%s | sent=%lu nack=%lu\n",
                          (unsigned long)ppgSnap.ir_raw,
                          edge.finger_on ? "YES" : "NO",
                          EdgeConfig::ENABLE_FINGER_GATE ? "ON" : "OFF",
                          sentCount, nackCount);
            Serial.printf("[DBG] ax=%.4f ay=%.4f az=%.4f | gx=%.4f gy=%.4f gz=%.4f\n",
                          imuSnap.accel_x, imuSnap.accel_y, imuSnap.accel_z,
                          imuSnap.gyro_x, imuSnap.gyro_y, imuSnap.gyro_z);
            lastDebugLog = millis();
        }

        if (shouldSend)
        {
            CombinedPacket pkt{};
            pkt.header.type      = PacketType::COMBINED_DATA;
            pkt.header.node_id   = NODE_ID;
            pkt.header.timestamp = millis();
            pkt.imu  = imuSnap;
            pkt.ppg  = ppgSnap;
            pkt.edge = edge;

            bool ok = g_espnow.sendCombined(pkt);
            if (ok)
            {
                sentCount++;
                Serial.printf("[TX] #%lu Node %d | IR=%lu finger=%s | HR=%d\n",
                              sentCount, NODE_ID,
                              (unsigned long)ppgSnap.ir_raw,
                              edge.finger_on ? "Y" : "N",
                              ppgSnap.heart_rate);
            }
            else
            {
                nackCount++;
            }
        }

        if (millis() - lastHeartbeat >= Timing::HEARTBEAT_MS)
        {
            HeartbeatPacket hb{};
            hb.header.type      = PacketType::HEARTBEAT;
            hb.header.node_id   = NODE_ID;
            hb.header.timestamp = millis();
            hb.uptime_s         = millis() / 1000;
            hb.rssi             = 0;
            g_espnow.sendHeartbeat(hb);
            lastHeartbeat = millis();
            Serial.printf("[HB] Heartbeat dikirim (uptime=%lus)\n", hb.uptime_s);
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::SEND_INTERVAL_MS));

        static uint32_t iter = 0;
        if (++iter % 500 == 0)
            g_watchdog.checkTaskStack("ESPNOW_TX");
    }
}

// ---------------------------------------------------------------------------
// taskMonitor — health check periodik (sensor)
// ---------------------------------------------------------------------------
static void taskMonitor(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        g_watchdog.checkTaskStack("PPG");
        g_watchdog.checkTaskStack("IMU");
        g_watchdog.checkTaskStack("ESPNOW_TX");

        if (esp_get_free_heap_size() < 20 * 1024)
            g_watchdog.triggerRestart("Heap < 20KB");

        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_SENSOR

// ===========================================================================
// GATEWAY NODE
// ===========================================================================
#if NODE_ROLE == ROLE_GATEWAY

static NetworkEspNow g_espnow;
static NetworkMqtt   g_mqtt;

// ---------------------------------------------------------------------------
// [Item #5] Batching state untuk taskSerialize
// Identik dengan yang sebelumnya ada di Network_EspNow.cpp (onDataRecv)
// ---------------------------------------------------------------------------
struct BatchBuffer {
    static constexpr uint8_t MAX_BATCH = 10;
    char    entries[MAX_BATCH][180];
    uint8_t count   = 0;
    uint8_t node_id = 0;
};
static BatchBuffer g_batchBuf[2]; // index 0 = node 1, index 1 = node 2

static inline uint8_t batchIdx(uint8_t node_id) {
    return (node_id >= 1 && node_id <= 2) ? (node_id - 1) : 0;
}

// ---------------------------------------------------------------------------
// [Item #5] taskSerialize — Core 1
//
// Memindahkan SEMUA logika dari onDataRecv ISR ke sini:
//   - Routing berdasarkan PacketType
//   - Serialisasi JSON (snprintf, loop CS_M)
//   - Batching MQTT
//   - Push ke g_mqttQueue
//
// Dengan begini ISR hanya ~1µs (memcpy), task ini boleh lambat sesuka hati.
// ---------------------------------------------------------------------------
static void taskSerialize(void *param)
{
    g_watchdog.registerTask();

    RawPacket raw{};
    uint32_t  receivedCount = 0;
    uint32_t  droppedCount  = 0;  // paket yang datang saat g_rawQueue penuh
    uint32_t  lastLog       = 0;

    Serial.println("[SERIALIZE] taskSerialize dimulai (Core 1)");

    for (;;)
    {
        g_watchdog.feed();

        // Tunggu paket dari ISR — blokir sampai ada
        if (xQueueReceive(g_rawQueue, &raw, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        receivedCount++;

        // Validasi minimal
        if (raw.len < 2) continue;

        PacketType type   = static_cast<PacketType>(raw.data[0]);
        uint8_t    nodeId = raw.data[1];

        // ── Log throughput setiap 10 detik ──────────────────────────────────
        if (millis() - lastLog >= 10000)
        {
            Serial.printf("[SERIALIZE] recv=%lu drop=%lu | rawQ=%u mqttQ=%u\n",
                          receivedCount, droppedCount,
                          uxQueueMessagesWaiting(g_rawQueue),
                          uxQueueMessagesWaiting(g_mqttQueue));
            lastLog = millis();
        }

        // ── Handle CombinedPacket ────────────────────────────────────────────
        if (type == PacketType::COMBINED_DATA)
        {
            if (raw.len < static_cast<int>(sizeof(CombinedPacket))) continue;
            const auto* pkt = reinterpret_cast<const CombinedPacket*>(raw.data);

            char entry[180];
            snprintf(entry, sizeof(entry),
                     "{"
                     "\"ts\":%lu,"
                     "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                     "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
                     "\"ir\":%lu,\"red\":%lu,"
                     "\"hr\":%d,\"spo2\":%.1f,\"ppg_valid\":%s,"
                     "\"finger\":%s"
                     "}",
                     pkt->header.timestamp,
                     pkt->imu.accel_x, pkt->imu.accel_y, pkt->imu.accel_z,
                     pkt->imu.gyro_x,  pkt->imu.gyro_y,  pkt->imu.gyro_z,
                     (unsigned long)pkt->ppg.ir_raw, (unsigned long)pkt->ppg.red_raw,
                     pkt->ppg.heart_rate, pkt->ppg.spo2,
                     pkt->ppg.valid      ? "true" : "false",
                     pkt->edge.finger_on ? "true" : "false");

            if (BatchConfig::BATCHING_ENABLED)
            {
                uint8_t idx      = batchIdx(nodeId);
                BatchBuffer& buf = g_batchBuf[idx];
                buf.node_id      = nodeId;

                uint8_t batchSize = (BatchConfig::BATCH_SIZE <= BatchBuffer::MAX_BATCH)
                                    ? BatchConfig::BATCH_SIZE
                                    : BatchBuffer::MAX_BATCH;

                if (buf.count < batchSize)
                {
                    strncpy(buf.entries[buf.count], entry, sizeof(buf.entries[0]) - 1);
                    buf.count++;
                }

                if (buf.count >= batchSize)
                {
                    MqttMessage msg{};
                    snprintf(msg.topic, sizeof(msg.topic),
                             "%s/node_%d/combined", Mqtt::TOPIC_BASE, nodeId);

                    int pos = 0;
                    msg.payload[pos++] = '[';
                    for (uint8_t i = 0; i < buf.count && pos < (int)sizeof(msg.payload) - 2; i++)
                    {
                        if (i > 0) msg.payload[pos++] = ',';
                        int rem     = sizeof(msg.payload) - pos - 2;
                        int written = snprintf(msg.payload + pos, rem, "%s", buf.entries[i]);
                        if (written > 0 && written < rem) pos += written;
                    }
                    msg.payload[pos++] = ']';
                    msg.payload[pos]   = '\0';

                    if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
                        droppedCount++;
                    buf.count = 0;
                }
            }
            else
            {
                MqttMessage msg{};
                snprintf(msg.topic, sizeof(msg.topic),
                         "%s/node_%d/combined", Mqtt::TOPIC_BASE, nodeId);
                snprintf(msg.payload, sizeof(msg.payload), "%s", entry);

                if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
                {
                    droppedCount++;
                    Serial.println("[SERIALIZE] WARN: mqttQueue penuh, paket dibuang!");
                }
            }
            continue;
        }

        // ── Handle Heartbeat ─────────────────────────────────────────────────
        if (type == PacketType::HEARTBEAT)
        {
            if (raw.len < static_cast<int>(sizeof(HeartbeatPacket))) continue;
            const auto* pkt = reinterpret_cast<const HeartbeatPacket*>(raw.data);

            MqttMessage msg{};
            snprintf(msg.topic, sizeof(msg.topic),
                     "%s/node_%d/heartbeat", Mqtt::TOPIC_BASE, nodeId);
            snprintf(msg.payload, sizeof(msg.payload),
                     "{\"ts\":%lu,\"uptime\":%lu}",
                     pkt->header.timestamp, (unsigned long)pkt->uptime_s);

            if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
                droppedCount++;
            continue;
        }

        // ── Handle CS packets (ax/ay/az/gx/gy/gz/ir) ────────────────────────
        uint8_t raw_type = static_cast<uint8_t>(type);
        if (raw_type >= PKT_CS_AX && raw_type <= PKT_CS_IR)
        {
            const char* axis_names[] = {"ax","ay","az","gx","gy","gz","ir"};
            uint8_t axis_idx         = raw_type - PKT_CS_AX;

            if (raw_type == PKT_CS_IR)
            {
                if (raw.len < static_cast<int>(sizeof(CSPpgPacket))) continue;
                const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);

                MqttMessage msg{};
                snprintf(msg.topic, sizeof(msg.topic),
                         "%s/node_%d/cs_ir", Mqtt::TOPIC_BASE, nodeId);

                char* p   = msg.payload;
                int   rem = sizeof(msg.payload);
                int   w   = snprintf(p, rem,
                            "{\"ts\":%lu,\"hr\":%d,\"ppg_valid\":%s,\"finger\":%s,\"y\":[",
                            pkt->header.timestamp, pkt->heart_rate,
                            pkt->ppg_valid      ? "true" : "false",
                            pkt->edge.finger_on ? "true" : "false");
                p += w; rem -= w;
                for (uint8_t i = 0; i < CS_M && rem > 15; i++)
                {
                    w = snprintf(p, rem, i ? ",%.5f" : "%.5f", pkt->y_ir[i]);
                    p += w; rem -= w;
                }
                snprintf(p, rem, "]}");

                if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
                    droppedCount++;
            }
            else
            {
                if (raw.len < static_cast<int>(sizeof(CS1AxisPacket))) continue;
                const auto* pkt = reinterpret_cast<const CS1AxisPacket*>(raw.data);

                MqttMessage msg{};
                snprintf(msg.topic, sizeof(msg.topic),
                         "%s/node_%d/cs_%s",
                         Mqtt::TOPIC_BASE, nodeId, axis_names[axis_idx]);

                char* p   = msg.payload;
                int   rem = sizeof(msg.payload);
                int   w   = snprintf(p, rem,
                            "{\"ts\":%lu,\"finger\":%s,\"y\":[",
                            pkt->header.timestamp,
                            pkt->edge.finger_on ? "true" : "false");
                p += w; rem -= w;
                for (uint8_t i = 0; i < CS_M && rem > 15; i++)
                {
                    w = snprintf(p, rem, i ? ",%.5f" : "%.5f", pkt->y[i]);
                    p += w; rem -= w;
                }
                snprintf(p, rem, "]}");

                if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
                    droppedCount++;
            }
            continue;
        }

        // Tipe tidak dikenal
        Serial.printf("[SERIALIZE] WARN: tipe paket tidak dikenal: 0x%02X\n",
                      static_cast<uint8_t>(type));
    }
}

// ---------------------------------------------------------------------------
// taskMqttPublish — Core 0 (tidak berubah)
// ---------------------------------------------------------------------------
static void taskMqttPublish(void *param)
{
    g_watchdog.registerTask();

    MqttMessage msg{};
    uint32_t publishedCount = 0;
    uint32_t failCount      = 0;
    uint32_t lastStatusLog  = 0;

    for (;;)
    {
        g_watchdog.feed();

        if (millis() - lastStatusLog >= 10000)
        {
            Serial.printf("[MQTT] Status: WiFi=%s MQTT=%s | pub=%lu fail=%lu queue=%u\n",
                          g_mqtt.isWifiConnected() ? "OK" : "DOWN",
                          g_mqtt.isConnected()     ? "OK" : "DOWN",
                          publishedCount, failCount,
                          uxQueueMessagesWaiting(g_mqttQueue));
            lastStatusLog = millis();
        }

        if (!g_mqtt.isConnected())
        {
            g_mqtt.loop();
            xQueueReceive(g_mqttQueue, &msg, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xQueueReceive(g_mqttQueue, &msg,
                          pdMS_TO_TICKS(Timing::MQTT_PUBLISH_MS)) == pdTRUE)
        {
            bool ok = g_mqtt.publish(msg.topic, msg.payload);
            if (ok)
            {
                publishedCount++;
                Serial.printf("[MQTT] #%lu → %s (%d B)\n",
                              publishedCount, msg.topic, strlen(msg.payload));
            }
            else
            {
                failCount++;
                if (failCount > 10 && publishedCount == 0)
                    g_watchdog.triggerRestart("MQTT publish fail 10x");
                Serial.printf("[MQTT] GAGAL #%lu → %s (rc=%d)\n",
                              failCount, msg.topic, g_mqtt.state());
            }
        }

        g_mqtt.loop();

        static uint32_t iter = 0;
        if (++iter % 100 == 0)
            g_watchdog.checkTaskStack("MQTT_PUB");
    }
}

// ---------------------------------------------------------------------------
// taskMonitorGateway — health check periodik
// ---------------------------------------------------------------------------
static void taskMonitorGateway(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();

        UBaseType_t queueUsed  = uxQueueMessagesWaiting(g_mqttQueue);
        UBaseType_t queueFree  = uxQueueSpacesAvailable(g_mqttQueue);
        UBaseType_t queueTotal = queueUsed + queueFree;
        float queueFillPct     = 100.0f * queueUsed / queueTotal;

        if (queueFillPct > 80.0f)
            Serial.printf("[MONITOR] ⚠ Queue MQTT: %.0f%% penuh (%d/%d)\n",
                          queueFillPct, queueUsed, queueTotal);

        // Cek rawQueue juga
        UBaseType_t rawUsed = uxQueueMessagesWaiting(g_rawQueue);
        UBaseType_t rawFree = uxQueueSpacesAvailable(g_rawQueue);
        if (rawUsed > 5)
            Serial.printf("[MONITOR] ⚠ rawQueue menumpuk: %d/%d\n",
                          rawUsed, rawUsed + rawFree);

        int8_t rssi = WiFi.RSSI();
        if (rssi < -85)
            Serial.printf("[MONITOR] ⚠ WiFi RSSI lemah: %d dBm\n", rssi);

        if (WiFi.status() != WL_CONNECTED)
        {
            static uint32_t wifiFailSince = 0;
            if (wifiFailSince == 0) wifiFailSince = millis();
            if (millis() - wifiFailSince > 30000)
                g_watchdog.triggerRestart("WiFi down 30s");
        }

        Serial.printf("[MONITOR] rawQ=%d/%d | mqttQ=%d/%d (%.0f%%) | "
                      "WiFi=%s RSSI=%d | Heap=%luKB\n",
                      rawUsed, rawUsed + rawFree,
                      queueUsed, queueTotal, queueFillPct,
                      WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                      rssi,
                      esp_get_free_heap_size() / 1024);

        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_GATEWAY

// ===========================================================================
// setup()
// ===========================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Health Monitor (ISR Offload + CombinedPacket) ===");
    Serial.printf("Node ID : %d\n", NODE_ID);
    Serial.printf("Role    : %s\n", (NODE_ROLE == ROLE_SENSOR) ? "SENSOR" : "GATEWAY");
    Serial.printf("Interval: %lu ms\n", (unsigned long)Timing::SEND_INTERVAL_MS);
    Serial.println("=====================================================\n");

    g_watchdog.begin(true);

// ---------------------------------------------------------------------------
#if NODE_ROLE == ROLE_SENSOR

    g_wire0Mutex = xSemaphoreCreateMutex();
    g_wire1Mutex = xSemaphoreCreateMutex();
    if (!g_wire0Mutex || !g_wire1Mutex)
        g_watchdog.triggerRestart("Mutex create fail");

    if (!g_imu.begin())
        g_watchdog.triggerRestart("MPU6050 init fail");

    Serial.printf("[CONFIG] Finger gate  : %s (threshold IR=%lu)\n",
                  EdgeConfig::ENABLE_FINGER_GATE ? "AKTIF" : "NONAKTIF",
                  (unsigned long)EdgeConfig::IR_FINGER_THRESHOLD);
    Serial.printf("[CONFIG] Send interval: %lu ms\n",
                  (unsigned long)Timing::SEND_INTERVAL_MS);

    // ESP-NOW HARUS sebelum Wire.begin() untuk MAX30102
    if (!g_espnow.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init fail");

    if (!g_ppg.begin())
    {
        Serial.println("[WARN] MAX30102 gagal. Lanjut tanpa PPG.");
        Serial.println("[WARN] Finger detection akan selalu false.");
    }

    xTaskCreatePinnedToCore(taskReadPPG,    "PPG",     StackSize::SENSOR_PPG,
                            nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskReadIMU,    "IMU",     StackSize::SENSOR_IMU,
                            nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender,   "CS_TX",   StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX, nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitor,    "MONITOR", StackSize::MONITOR,
                            nullptr, 1, nullptr, 0);

    Serial.println("[SETUP] Sensor node siap.");

// ---------------------------------------------------------------------------
#elif NODE_ROLE == ROLE_GATEWAY

    Serial.printf("[CONFIG] Batching: %s",
                  BatchConfig::BATCHING_ENABLED ? "AKTIF" : "NONAKTIF");
    if (BatchConfig::BATCHING_ENABLED)
        Serial.printf(" (size=%d, latensi ~%lu ms)",
                      BatchConfig::BATCH_SIZE,
                      (unsigned long)(BatchConfig::BATCH_SIZE * Timing::SEND_INTERVAL_MS));
    Serial.println();

    // ── Buat kedua queue sebelum begin() apapun ──────────────────────────────
    // g_rawQueue: buffer ISR → taskSerialize
    //   Size 10 cukup: taskSerialize cepat, ISR hanya 7 paket/window (~7ms)
    //   RAM: 257 × 10 = 2.57 KB
    g_rawQueue = xQueueCreate(10, sizeof(RawPacket));
    if (!g_rawQueue)
        g_watchdog.triggerRestart("rawQueue create fail");
    Serial.printf("[SETUP] rawQueue dibuat (10 × %d bytes = %d bytes)\n",
                  sizeof(RawPacket), 10 * sizeof(RawPacket));

    // g_mqttQueue: taskSerialize → taskMqttPublish
    //   RAM: 500 × 30 = 15 KB
    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));
    if (!g_mqttQueue)
        g_watchdog.triggerRestart("mqttQueue create fail");
    Serial.printf("[SETUP] mqttQueue dibuat (%d × %d bytes)\n",
                  QueueLen::MQTT_MSG, sizeof(MqttMessage));

    if (!g_mqtt.begin())
        g_watchdog.triggerRestart("WiFi/MQTT init fail");

    if (!g_espnow.begin(false))
        g_watchdog.triggerRestart("ESP-NOW init fail");

    // taskSerialize di Core 1 — pisah dari taskMqttPublish (Core 0)
    // Prioritas sedikit lebih tinggi dari MQTT agar queue tidak overflow
    xTaskCreatePinnedToCore(taskSerialize,      "SERIALIZE", StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB + 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskMqttPublish,    "MQTT",      StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB,     nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorGateway, "MONITOR",   StackSize::MONITOR,
                            nullptr, 1,                      nullptr, 0);

    Serial.println("[SETUP] Gateway siap.");
    Serial.println("[SETUP] Pipeline: ISR → rawQueue → taskSerialize → mqttQueue → taskMqttPublish");

#endif
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}
