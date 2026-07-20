// File: firmware/src/main.cpp

// =============================================================================
// main.cpp — Orkestrator v4.0 (Boot-Anytime)
// =============================================================================
//
// PERBAIKAN v4.0:
//   Sensor boot NON-BLOCKING — begin(true) langsung return,
//   channel discovery jalan di background FreeRTOS task.
//   Gateway dan sensor bisa dinyalakan di waktu BERBEDA.
//
//   Task CS_TX dan RSSI_EX menunggu isChannelConfirmed() sebelum kirim data.
//   Task IMU dan PPG langsung jalan (baca sensor tanpa perlu koneksi).
//
// PERBAIKAN v3.1:
//   [FIX-1] Gateway: g_mesh.setGatewayChannel(ch) dipanggil setelah
//           g_mqtt.begin() agar peer ESP-NOW menggunakan channel WiFi
//           yang benar. Sebelumnya channel = 0 → LoadProhibited crash.
//
// URUTAN INISIALISASI GATEWAY:
//   1. g_mqtt.begin()                 ← WiFi connect → dapat channel asli
//   2. g_mesh.begin(false)            ← ESP-NOW init, peer ch dari WiFi
//   3. g_mesh.setGatewayChannel(ch)   ← update semua peer ke channel WiFi asli
//   4. xTaskCreatePinnedToCore(BEACON, HANDLER, MQTT, MONITOR)
//
// URUTAN INISIALISASI SENSOR (v4.0 — non-blocking):
//   1. g_imu.begin()
//   2. g_mesh.begin(true)   ← ESP-NOW init ch=1, return SEGERA
//                              (background task sweep channel)
//   3. g_ppg.begin()
//   4. Semua task dimulai — CS_TX dan RSSI_EX tunggu channel confirmed
// =============================================================================

#include <Arduino.h>
#include <esp_wifi.h>
#include "Config.h"
#include "MeshPackets.h"

#include "Watchdog.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "EspNowMesh.h"
#include "Network_Mqtt.h"
#include "DynamicRouter.h"

extern void taskCSSender(void *param);
extern void taskRssiExchange(void *param);
extern void taskMeshHandler(void *param);
extern void taskMqttPublish(void *param);

extern DynamicRouter* g_routerPtr;
extern QueueHandle_t g_mqttQueue;

static constexpr char TAG[] = "MAIN";

// =============================================================================
// Instance & Shared State Global
// =============================================================================

// I2C mutex tetap dipakai: taskCSSender membaca sensor langsung
// (single synchronous pipeline), dan taskSensorReceiver bisa pakai bus juga.
SemaphoreHandle_t g_wireMutex = nullptr;

// NOTE: g_imu / g_ppg TIDAK boleh `static` — diakses via extern dari
// task_cs_sender.cpp. `static` = file-local → gagal link.
#if NODE_ROLE == ROLE_SENSOR_IMU
SensorMPU g_imu;
EspNowMesh g_mesh;
#elif NODE_ROLE == ROLE_SENSOR_PPG
SensorPPG g_ppg;
EspNowMesh g_mesh;
#endif

#if NODE_ROLE == ROLE_GATEWAY
EspNowMesh g_mesh;
NetworkMqtt g_mqtt;
DynamicRouter *g_routerPtr = nullptr; // gateway tidak punya router
#endif

// =============================================================================
// SENSOR NODE — Tasks
// =============================================================================

#if NODE_ROLE != ROLE_GATEWAY
static void taskSensorReceiver(void *param)
{
    g_watchdog.registerTask();
    RawPacket raw{};
    for (;;)
    {
        g_watchdog.feed();
        if (g_mesh.readPacket(raw))
        {
            const uint8_t pktType = raw.data[0];
            if (pktType == static_cast<uint8_t>(PacketType::RSSI_REPORT))
            {
                if (raw.len >= static_cast<int>(sizeof(RssiReportPacket)) && g_routerPtr)
                {
                    const auto* pkt = reinterpret_cast<const RssiReportPacket*>(raw.data);
                    g_routerPtr->updateNeighborRssi(pkt->header.nodeId, pkt->rssiToGateway);
                }
            }
            else if (pktType >= static_cast<uint8_t>(PacketType::CS_AX) && 
                     pktType <= static_cast<uint8_t>(PacketType::CS_IR))
            {
                // Baca nodeId pengirim asli via cast eksplisit (bukan byte mentah)
                const auto* hdr = reinterpret_cast<const PacketHeader*>(raw.data);
                const uint8_t originalNodeId = hdr->nodeId;

                // Anti-loop: jangan relay paket yang sumber aslinya kita sendiri.
                if (originalNodeId == NODE_ID)
                {
                    LOG_EVERY_N(20, LOG_WARN, TAG,
                                "Skip relay: paket asal node sendiri (loop guard)");
                }
                else
                {
                    // Relay packet dari neighbor
                    g_mesh.forwardRoutedCs(NODE_ID, originalNodeId, raw.data, raw.len);
                }
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void taskMonitorSensor(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        // taskReadIMU/taskReadPPG sudah dihapus (single synchronous pipeline) —
        // pembacaan sensor kini di dalam CS_TX, jadi cukup pantau CS_TX & RSSI_EX.
        g_watchdog.checkTaskStack("CS_TX");
        g_watchdog.checkTaskStack("RSSI_EX");
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}
#endif

// =============================================================================
// GATEWAY NODE — Tasks
// =============================================================================
#if NODE_ROLE == ROLE_GATEWAY

static void taskBeacon(void *param)
{
    static constexpr char BTAG[] = "BEACON";
    
    // Fase awal: beacon agresif 200ms selama 30 detik pertama
    // Setelah itu kembali ke interval normal
    const uint32_t startMs       = millis();
    const uint32_t FAST_DURATION = 30000;   // 30 detik
    const uint32_t FAST_INTERVAL = 200;     // ms
    const uint32_t NORMAL_INTERVAL = static_cast<uint32_t>(
                                        RoutingCfg::BEACON_INTERVAL_MS);

    LOG_INFO(BTAG, "taskBeacon dimulai | normal interval=%lu ms", (unsigned long)NORMAL_INTERVAL);

    for (;;)
    {
        const bool ok = g_mesh.sendBeacon();
        LOG_EVERY_N(10, LOG_DEBUG, BTAG, "Beacon | ok=%s", ok ? "Y" : "N");

        const uint32_t interval = (millis() - startMs < FAST_DURATION)
                                  ? FAST_INTERVAL
                                  : NORMAL_INTERVAL;
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

static void taskMonitorGateway(void *param)
{
    uint32_t wifiDownSince = 0;

    for (;;)
    {
        g_watchdog.healthCheck();

        UBaseType_t rawUsed = 0, rawFree = 0;
        g_mesh.getQueueMetrics(rawUsed, rawFree);
        const UBaseType_t mqttUsed = uxQueueMessagesWaiting(g_mqttQueue);
        const UBaseType_t mqttFree = uxQueueSpacesAvailable(g_mqttQueue);
        const float mqttPct = 100.0f * mqttUsed / (mqttUsed + mqttFree);

        if (mqttPct > 80.0f)
            LOG_WARN(TAG, "mqttQueue %.0f%% penuh", mqttPct);

        if (!g_mqtt.isWifiConnected())
        {
            if (wifiDownSince == 0)
                wifiDownSince = millis();
            if (millis() - wifiDownSince > 30000)
                g_watchdog.triggerRestart("WiFi down > 30s");
        }
        else
        {
            wifiDownSince = 0;
        }

        LOG_INFO(TAG,
                 "rawQ=%u/%u | mqttQ=%u/%u (%.0f%%) | WiFi=%s | heap=%lu KB",
                 rawUsed, rawUsed + rawFree,
                 mqttUsed, mqttUsed + mqttFree, mqttPct,
                 g_mqtt.isWifiConnected() ? "OK" : "DOWN",
                 esp_get_free_heap_size() / 1024);

        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_GATEWAY

static void startTaskOrRestart(TaskFunction_t task,
                               const char* name,
                               uint32_t stackDepth,
                               UBaseType_t priority,
                               BaseType_t core)
{
    if (xTaskCreatePinnedToCore(task, name, stackDepth, nullptr,
                                priority, nullptr, core) != pdPASS)
    {
        char reason[64];
        snprintf(reason, sizeof(reason), "Gagal buat task %s", name);
        g_watchdog.triggerRestart(reason);
    }
}

// =============================================================================
// setup()
// =============================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    LOG_INFO(TAG, "================================================");
    LOG_INFO(TAG, "  Health Monitor Mesh v3.1 — Multi-Hop Routing");
    LOG_INFO(TAG, "  Node %d | Role: %s",
             NODE_ID,
             (NODE_ROLE == ROLE_SENSOR_IMU) ? "SENSOR IMU" :
             (NODE_ROLE == ROLE_SENSOR_PPG) ? "SENSOR PPG" : "GATEWAY");
    LOG_INFO(TAG, "  Discovery: %lu ms | Beacon: %lu ms | Threshold: %d dBm",
             (unsigned long)RoutingCfg::DISCOVERY_PHASE_MS,
             (unsigned long)RoutingCfg::BEACON_INTERVAL_MS,
             RoutingCfg::RELAY_THRESHOLD_DBM);
    LOG_INFO(TAG, "================================================");

    g_watchdog.begin(true);

// ── SENSOR NODE IMU ──────────────────────────────────────────────────────────
#if NODE_ROLE == ROLE_SENSOR_IMU

    g_wireMutex = xSemaphoreCreateMutex();
    if (!g_wireMutex)
        g_watchdog.triggerRestart("Gagal buat I2C mutex");

    if (!g_imu.begin())
        g_watchdog.triggerRestart("MPU6050 init gagal");

    if (!g_mesh.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    startTaskOrRestart(taskCSSender, "CS_TX", StackSize::ESPNOW_TX,
                       TaskPrio::ESPNOW_TX, 0);
    startTaskOrRestart(taskRssiExchange, "RSSI_EX", 4096, 1, 0);
    startTaskOrRestart(taskMonitorSensor, "MONITOR", StackSize::MONITOR, 1, 0);
    startTaskOrRestart(taskSensorReceiver, "RX", 4096,
                       TaskPrio::ESPNOW_TX + 1, 1);

    LOG_INFO(TAG, "Sensor IMU node siap — 4 task terdaftar");

// ── SENSOR NODE PPG ──────────────────────────────────────────────────────────
#elif NODE_ROLE == ROLE_SENSOR_PPG

    g_wireMutex = xSemaphoreCreateMutex();
    if (!g_wireMutex)
        g_watchdog.triggerRestart("Gagal buat I2C mutex");

    if (!g_mesh.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    if (!g_ppg.begin())
        g_watchdog.triggerRestart("MAX30102 init gagal");

    startTaskOrRestart(taskCSSender, "CS_TX", StackSize::ESPNOW_TX,
                       TaskPrio::ESPNOW_TX, 0);
    startTaskOrRestart(taskRssiExchange, "RSSI_EX", 4096, 1, 0);
    startTaskOrRestart(taskMonitorSensor, "MONITOR", StackSize::MONITOR, 1, 0);
    startTaskOrRestart(taskSensorReceiver, "RX", 4096,
                       TaskPrio::ESPNOW_TX + 1, 1);

    LOG_INFO(TAG, "Sensor PPG node siap — 4 task terdaftar");

// ── GATEWAY NODE ─────────────────────────────────────────────────────────────
#elif NODE_ROLE == ROLE_GATEWAY

    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));
    if (!g_mqttQueue)
        g_watchdog.triggerRestart("Gagal buat queue");

    // ── URUTAN KRITIS: WiFi/MQTT DULU, baru ESP-NOW ──────────────────────────
    // WiFi.mode(AP_STA) di dalam g_mqtt.begin() harus terjadi SEBELUM
    // esp_now_init() di dalam g_mesh.begin().
    // Jika dibalik, WiFi.mode() akan reset state ESP-NOW.

    if (!g_mqtt.begin())
        g_watchdog.triggerRestart("WiFi/MQTT init gagal");

    if (!g_mesh.begin(false))   // ← ESP-NOW init setelah WiFi stabil
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    // Channel sudah benar karena begin(false) membaca dari WiFi aktif
    {
        uint8_t ch = 0; wifi_second_chan_t sch;
        if (esp_wifi_get_channel(&ch, &sch) != ESP_OK || ch == 0)
            ch = static_cast<uint8_t>(WiFi.channel());
        if (ch > 0 && ch <= 13)
        {
            g_mesh.setGatewayChannel(ch);
            LOG_INFO(TAG, "Gateway channel dikunci ke ch=%d", ch);
        }
    }

    startTaskOrRestart(taskBeacon, "BEACON", 4096, 1, 0);
    startTaskOrRestart(taskMeshHandler, "HANDLER", StackSize::MQTT_PUB,
                       TaskPrio::MQTT_PUB + 1, 1);
    startTaskOrRestart(taskMqttPublish, "MQTT", StackSize::MQTT_PUB,
                       TaskPrio::MQTT_PUB, 0);
    startTaskOrRestart(taskMonitorGateway, "MONITOR", StackSize::MONITOR, 1, 0);

    LOG_INFO(TAG, "Gateway siap — 4 task aktif");

#endif
}

// =============================================================================
// loop() — kosong, semua di FreeRTOS task
// =============================================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}
