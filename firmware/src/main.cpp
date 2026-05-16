// File: firmware/src/main.cpp

// =============================================================================
// main.cpp — Orkestrator v3.0 (Multi-Hop Dynamic Routing)
// =============================================================================
//
// PERUBAHAN v3.0:
//   SENSOR NODE:
//     + taskRssiExchange  → kirim RSSI ke neighbor setiap RSSI_EXCHANGE_MS
//     + g_routerPtr (extern) → diakses EspNowMesh saat terima RssiReport
//       dan saat node berperan sebagai RELAY (forward paket neighbor)
//
//   GATEWAY NODE:
//     + taskBeacon → broadcast beacon setiap BEACON_INTERVAL_MS
//       agar sensor node bisa mengukur RSSI ke gateway
//
// RELAY OPERATION (sensor node):
//   Saat sensor node terima CS packet dari neighbor (bukan dari gateway),
//   EspNowMesh._onDataRecv() mendeteksi src MAC = neighbor, bukan gateway,
//   lalu otomatis memanggil g_mesh.forwardRoutedCs() → wrap + kirim ke gateway.
//   Ini terjadi di ISR context, sangat cepat.
//
// URUTAN INISIALISASI (tidak berubah dari v2):
//   Sensor: IMU → ESP-NOW → PPG
//   Gateway: queue → WiFi/MQTT → ESP-NOW
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "MeshPackets.h"

#include "Watchdog.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "EspNowMesh.h"
#include "Network_Mqtt.h"

extern void taskCSSender     (void* param);
extern void taskRssiExchange (void* param);   // ← BARU (sensor)
extern void taskMeshHandler  (void* param);
extern void taskMqttPublish  (void* param);

static constexpr char TAG[] = "MAIN";


// =============================================================================
// Instance & Shared State Global
// =============================================================================

portMUX_TYPE g_stateMux  = portMUX_INITIALIZER_UNLOCKED;
ImuSample    g_latestImu = {};
PpgSample    g_latestPpg = {};

SemaphoreHandle_t g_wire0Mutex = nullptr;
SemaphoreHandle_t g_wire1Mutex = nullptr;

#if NODE_ROLE == ROLE_SENSOR
    static SensorMPU   g_imu;
    static SensorPPG   g_ppg;
    EspNowMesh         g_mesh;
#endif

#if NODE_ROLE == ROLE_GATEWAY
    static EspNowMesh  g_mesh;
    NetworkMqtt        g_mqtt;
#endif


// =============================================================================
// SENSOR NODE — Tasks
// =============================================================================
#if NODE_ROLE == ROLE_SENSOR

static void taskReadPPG(void* param)
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
            LOG_WARN(TAG, "Wire0 mutex timeout di taskReadPPG");
        }

        PpgSample snap{};
        g_ppg.read(snap);

        taskENTER_CRITICAL(&g_stateMux);
        g_latestPpg = snap;
        taskEXIT_CRITICAL(&g_stateMux);

        taskYIELD();
    }
}

static void taskReadIMU(void* param)
{
    g_watchdog.registerTask();

    uint32_t lastReadMs = 0;
    uint8_t  failCount  = 0;

    for (;;)
    {
        g_watchdog.feed();

        if (millis() - lastReadMs >= Timing::IMU_SAMPLE_MS)
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

            lastReadMs = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void taskMonitorSensor(void* param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        g_watchdog.checkTaskStack("PPG");
        g_watchdog.checkTaskStack("IMU");
        g_watchdog.checkTaskStack("CS_TX");
        g_watchdog.checkTaskStack("RSSI_EX");
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_SENSOR


// =============================================================================
// GATEWAY NODE — taskBeacon (BARU) + taskMonitor
// =============================================================================
#if NODE_ROLE == ROLE_GATEWAY

// ---------------------------------------------------------------------------
// taskBeacon — Broadcast beacon periodik untuk RSSI discovery sensor node
//
// Sensor node mengukur RSSI dari beacon ini untuk menentukan seberapa
// dekat mereka ke gateway. Beacon dikirim via ESP-NOW broadcast.
//
// Interval: BEACON_INTERVAL_MS (default 1000ms = 1 Hz)
// Core: 0 (ringan, tidak butuh core dedicated)
// Prioritas: rendah — tidak boleh ganggu MQTT publish
// ---------------------------------------------------------------------------
static void taskBeacon(void* param)
{
    static constexpr char BTAG[] = "BEACON";

    LOG_INFO(BTAG, "taskBeacon dimulai | interval=%lu ms",
             (unsigned long)RoutingCfg::BEACON_INTERVAL_MS);

    for (;;)
    {
        const bool ok = g_mesh.sendBeacon();

        LOG_EVERY_N(10, LOG_DEBUG, BTAG,
                    "Beacon sent | ok=%s", ok ? "Y" : "N");

        vTaskDelay(pdMS_TO_TICKS(RoutingCfg::BEACON_INTERVAL_MS));
    }
}

static void taskMonitorGateway(void* param)
{
    for (;;)
    {
        g_watchdog.healthCheck();

        const UBaseType_t rawUsed  = uxQueueMessagesWaiting(g_rawQueue);
        const UBaseType_t rawFree  = uxQueueSpacesAvailable(g_rawQueue);
        const UBaseType_t mqttUsed = uxQueueMessagesWaiting(g_mqttQueue);
        const UBaseType_t mqttFree = uxQueueSpacesAvailable(g_mqttQueue);
        const float       mqttPct  = 100.0f * mqttUsed / (mqttUsed + mqttFree);

        if (mqttPct > 80.0f)
            LOG_WARN(TAG, "mqttQueue %.0f%% penuh", mqttPct);

        if (!g_mqtt.isWifiConnected())
        {
            static uint32_t wifiDownSince = 0;
            if (wifiDownSince == 0) wifiDownSince = millis();
            if (millis() - wifiDownSince > 30000)
                g_watchdog.triggerRestart("WiFi down > 30s");
        }

        LOG_INFO(TAG,
                 "rawQ=%u/%u | mqttQ=%u/%u (%.0f%%) | WiFi=%s | heap=%lu KB",
                 rawUsed,  rawUsed  + rawFree,
                 mqttUsed, mqttUsed + mqttFree, mqttPct,
                 g_mqtt.isWifiConnected() ? "OK" : "DOWN",
                 esp_get_free_heap_size() / 1024);

        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_GATEWAY


// =============================================================================
// setup()
// =============================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    LOG_INFO(TAG, "================================================");
    LOG_INFO(TAG, "  Health Monitor Mesh v3.0 — Multi-Hop Routing");
    LOG_INFO(TAG, "  Node %d | Role: %s",
             NODE_ID,
             (NODE_ROLE == ROLE_SENSOR) ? "SENSOR" : "GATEWAY");
    LOG_INFO(TAG, "  Discovery: %lu ms | Beacon: %lu ms | Threshold: %d dBm",
             (unsigned long)RoutingCfg::DISCOVERY_PHASE_MS,
             (unsigned long)RoutingCfg::BEACON_INTERVAL_MS,
             RoutingCfg::RELAY_THRESHOLD_DBM);
    LOG_INFO(TAG, "================================================");

    g_watchdog.begin(true);

// ── SENSOR NODE ──────────────────────────────────────────────────────────────
#if NODE_ROLE == ROLE_SENSOR

    g_wire0Mutex = xSemaphoreCreateMutex();
    g_wire1Mutex = xSemaphoreCreateMutex();
    if (!g_wire0Mutex || !g_wire1Mutex)
        g_watchdog.triggerRestart("Gagal buat I2C mutex");

    if (!g_imu.begin())
        g_watchdog.triggerRestart("MPU6050 init gagal");

    if (!g_mesh.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    if (!g_ppg.begin())
        LOG_WARN(TAG, "MAX30102 gagal — lanjut tanpa PPG");

    // Sensor tasks
    xTaskCreatePinnedToCore(taskReadPPG,      "PPG",     StackSize::SENSOR_PPG,
                            nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskReadIMU,      "IMU",     StackSize::SENSOR_IMU,
                            nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender,     "CS_TX",   StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX,  nullptr, 0);

    // BARU: task kirim RSSI ke neighbor
    xTaskCreatePinnedToCore(taskRssiExchange, "RSSI_EX", 4096,
                            nullptr, 1,                    nullptr, 0);

    xTaskCreatePinnedToCore(taskMonitorSensor,"MONITOR", StackSize::MONITOR,
                            nullptr, 1,                    nullptr, 0);

    LOG_INFO(TAG, "Sensor node siap — 5 task terdaftar (termasuk RSSI_EX)");

// ── GATEWAY NODE ─────────────────────────────────────────────────────────────
#elif NODE_ROLE == ROLE_GATEWAY

    g_rawQueue = xQueueCreate(10, sizeof(RawPacket));
    if (!g_rawQueue)
        g_watchdog.triggerRestart("Gagal buat g_rawQueue");

    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));
    if (!g_mqttQueue)
        g_watchdog.triggerRestart("Gagal buat g_mqttQueue");

    if (!g_mqtt.begin())
        g_watchdog.triggerRestart("WiFi/MQTT init gagal");

    if (!g_mesh.begin(false))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    // BARU: task beacon untuk RSSI discovery
    xTaskCreatePinnedToCore(taskBeacon,         "BEACON",  2048,
                            nullptr, 1,                      nullptr, 0);

    xTaskCreatePinnedToCore(taskMeshHandler,    "HANDLER", StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB + 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskMqttPublish,    "MQTT",    StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB,     nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorGateway, "MONITOR", StackSize::MONITOR,
                            nullptr, 1,                      nullptr, 0);

    LOG_INFO(TAG, "Gateway siap — 4 task (+ BEACON)");
    LOG_INFO(TAG, "Pipeline: ISR → rawQ → HANDLER → mqttQ → MQTT");
    LOG_INFO(TAG, "Beacon: broadcast setiap %lu ms",
             (unsigned long)RoutingCfg::BEACON_INTERVAL_MS);

#endif
}


// =============================================================================
// loop() — kosong
// =============================================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}
