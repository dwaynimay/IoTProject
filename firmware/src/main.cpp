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

portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;
ImuSample g_latestImu = {};
PpgSample g_latestPpg = {};

SemaphoreHandle_t g_wireMutex = nullptr;

#if NODE_ROLE == ROLE_SENSOR_IMU
static SensorMPU g_imu;
EspNowMesh g_mesh;
#elif NODE_ROLE == ROLE_SENSOR_PPG
static SensorPPG g_ppg;
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
#endif

#if NODE_ROLE == ROLE_SENSOR_PPG
static void taskReadPPG(void *param)
{
    g_watchdog.registerTask();

    for (;;)
    {
        g_watchdog.feed();

        if (xSemaphoreTake(g_wireMutex, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            g_ppg.update();
            xSemaphoreGive(g_wireMutex);
        }
        else
        {
            LOG_WARN(TAG, "Wire mutex timeout di taskReadPPG");
        }

        PpgMeasurement snap{};
        g_ppg.read(snap);

        taskENTER_CRITICAL(&g_stateMux);
        g_latestPpg.irRaw = snap.irRaw;
        g_latestPpg.redRaw = snap.redRaw;
        g_latestPpg.heartRate = snap.heartRate;
        g_latestPpg.spo2 = snap.spo2;
        g_latestPpg.valid = snap.valid;
        taskEXIT_CRITICAL(&g_stateMux);

        vTaskDelay(pdMS_TO_TICKS(2)); 
    }
}

static void taskMonitorSensor(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        g_watchdog.checkTaskStack("PPG");
        g_watchdog.checkTaskStack("CS_TX");
        g_watchdog.checkTaskStack("RSSI_EX");
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}
#endif // ROLE_SENSOR_PPG

#if NODE_ROLE == ROLE_SENSOR_IMU
static void taskReadIMU(void *param)
{
    g_watchdog.registerTask();

    uint32_t lastReadMs = 0;
    uint8_t failCount = 0;

    for (;;)
    {
        g_watchdog.feed();

        if (millis() - lastReadMs >= Timing::IMU_SAMPLE_MS)
        {
            ImuMeasurement snap{};
            bool ok = false;

            if (xSemaphoreTake(g_wireMutex, portMAX_DELAY) == pdTRUE)
            {
                ok = g_imu.read(snap);
                xSemaphoreGive(g_wireMutex);
            }

            if (ok)
            {
                failCount = 0;
                taskENTER_CRITICAL(&g_stateMux);
                g_latestImu.accelX = snap.accelX;
                g_latestImu.accelY = snap.accelY;
                g_latestImu.accelZ = snap.accelZ;
                g_latestImu.gyroX = snap.gyroX;
                g_latestImu.gyroY = snap.gyroY;
                g_latestImu.gyroZ = snap.gyroZ;
                g_latestImu.tempC = snap.tempC;
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

static void taskMonitorSensor(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        g_watchdog.checkTaskStack("IMU");
        g_watchdog.checkTaskStack("CS_TX");
        g_watchdog.checkTaskStack("RSSI_EX");
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}
#endif // ROLE_SENSOR_IMU

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
            static uint32_t wifiDownSince = 0;
            if (wifiDownSince == 0)
                wifiDownSince = millis();
            if (millis() - wifiDownSince > 30000)
                g_watchdog.triggerRestart("WiFi down > 30s");
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

    xTaskCreatePinnedToCore(taskReadIMU, "IMU", StackSize::SENSOR_IMU,
                            nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender, "CS_TX", StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX, nullptr, 0);
    xTaskCreatePinnedToCore(taskRssiExchange, "RSSI_EX", 4096,
                            nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorSensor, "MONITOR", StackSize::MONITOR,
                            nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskSensorReceiver, "RX", 4096,
                            nullptr, TaskPrio::ESPNOW_TX + 1, nullptr, 1);

    LOG_INFO(TAG, "Sensor IMU node siap — 5 task terdaftar");

// ── SENSOR NODE PPG ──────────────────────────────────────────────────────────
#elif NODE_ROLE == ROLE_SENSOR_PPG

    g_wireMutex = xSemaphoreCreateMutex();
    if (!g_wireMutex)
        g_watchdog.triggerRestart("Gagal buat I2C mutex");

    if (!g_mesh.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    if (!g_ppg.begin())
        g_watchdog.triggerRestart("MAX30102 init gagal");

    xTaskCreatePinnedToCore(taskReadPPG, "PPG", StackSize::SENSOR_PPG,
                            nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender, "CS_TX", StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX, nullptr, 0);
    xTaskCreatePinnedToCore(taskRssiExchange, "RSSI_EX", 4096,
                            nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorSensor, "MONITOR", StackSize::MONITOR,
                            nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskSensorReceiver, "RX", 4096,
                            nullptr, TaskPrio::ESPNOW_TX + 1, nullptr, 1);

    LOG_INFO(TAG, "Sensor PPG node siap — 5 task terdaftar");

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

    xTaskCreatePinnedToCore(taskBeacon,       "BEACON",  4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskMeshHandler,  "HANDLER", StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB + 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskMqttPublish,  "MQTT",    StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB,     nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorGateway,"MONITOR", StackSize::MONITOR,
                            nullptr, 1, nullptr, 0);

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