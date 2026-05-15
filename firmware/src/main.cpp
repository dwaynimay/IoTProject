// File: firmware/src/main.cpp

// =============================================================================
// main.cpp — Orkestrator: Inisialisasi & Registrasi FreeRTOS Task
// =============================================================================
//
// PERUBAHAN v2 (refactor):
//   - taskSendEspNow (non-CS) dihapus dari sensor node. Mode CS adalah
//     satu-satunya mode yang didukung. Jika perlu mode non-CS kembali,
//     tambahkan #define USE_CS_MODE di features.h dan wrap dengan #ifdef.
//   - g_mqttQueue dipindah ke EspNowMesh.cpp (tidak didefinisikan di sini).
//     main.cpp hanya membuat queue via xQueueCreate dan assign ke extern.
//
// ATURAN FILE INI:
//   ✓ Deklarasi instance global (sensor, network, dsb.)
//   ✓ setup(): inisialisasi semua modul, buat queue, registrasi task
//   ✓ loop(): hanya vTaskDelay
//   ✗ JANGAN taruh logika task di sini
//   ✗ JANGAN taruh logika bisnis di sini
//
// URUTAN INISIALISASI (jangan diubah):
//   Sensor:
//     1. IMU (Wire1)  → tidak bergantung apapun
//     2. ESP-NOW      → harus sebelum Wire (PPG)
//     3. PPG (Wire)   → harus setelah ESP-NOW
//   Gateway:
//     1. Buat queue   → harus sebelum mesh.begin() (ISR butuh g_rawQueue)
//     2. MQTT + WiFi  → harus sebelum esp_now_init
//     3. ESP-NOW      → setelah WiFi konek
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "MeshPackets.h"

#include "Watchdog.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "EspNowMesh.h"
#include "Network_Mqtt.h"

extern void taskCSSender    (void* param);
extern void taskMeshHandler (void* param);
extern void taskMqttPublish (void* param);

static constexpr char TAG[] = "MAIN";


// =============================================================================
// Instance & Shared State Global
// =============================================================================

portMUX_TYPE g_stateMux  = portMUX_INITIALIZER_UNLOCKED;
ImuSample    g_latestImu = {};
PpgSample    g_latestPpg = {};

SemaphoreHandle_t g_wire0Mutex = nullptr;  // Wire  — MAX30102 (pin 18/19)
SemaphoreHandle_t g_wire1Mutex = nullptr;  // Wire1 — MPU6050  (pin 21/22)

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
// SENSOR NODE — Task Definitions
// =============================================================================
#if NODE_ROLE == ROLE_SENSOR

// ---------------------------------------------------------------------------
// taskReadPPG — Core 1, prioritas tertinggi
// ---------------------------------------------------------------------------
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
            LOG_WARN(TAG, "Wire0 mutex timeout 200ms di taskReadPPG");
        }

        PpgSample snap{};
        g_ppg.read(snap);

        taskENTER_CRITICAL(&g_stateMux);
        g_latestPpg = snap;
        taskEXIT_CRITICAL(&g_stateMux);

        LOG_EVERY_N(500, LOG_DEBUG, "PPG",
                    "IR=%lu | HR=%d | valid=%s | stack=%u",
                    (unsigned long)snap.irRaw, snap.heartRate,
                    snap.valid ? "Y" : "N",
                    uxTaskGetStackHighWaterMark(NULL));

        taskYIELD();
    }
}

// ---------------------------------------------------------------------------
// taskReadIMU — Core 1
// ---------------------------------------------------------------------------
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
                LOG_EVERY_N(10, LOG_WARN, "IMU",
                            "Read gagal %d kali berturut-turut", failCount);
                if (failCount > 50)
                    g_watchdog.triggerRestart("IMU read fail 50x berturut-turut");
            }

            lastReadMs = millis();
        }

        LOG_EVERY_N(500, LOG_DEBUG, "IMU",
                    "ax=%.3f ay=%.3f az=%.3f | stack=%u",
                    g_latestImu.accelX, g_latestImu.accelY, g_latestImu.accelZ,
                    uxTaskGetStackHighWaterMark(NULL));

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// taskMonitorSensor — Core 0
// ---------------------------------------------------------------------------
static void taskMonitorSensor(void* param)
{
    for (;;)
    {
        g_watchdog.healthCheck();
        g_watchdog.checkTaskStack("PPG");
        g_watchdog.checkTaskStack("IMU");
        g_watchdog.checkTaskStack("CS_TX");
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_SENSOR


// =============================================================================
// GATEWAY NODE — Task Monitor
// =============================================================================
#if NODE_ROLE == ROLE_GATEWAY

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
            LOG_WARN(TAG, "g_mqttQueue %.0f%% penuh (%u/%u)",
                     mqttPct, mqttUsed, mqttUsed + mqttFree);

        if (rawUsed > 5)
            LOG_WARN(TAG, "g_rawQueue menumpuk: %u/%u", rawUsed, rawUsed + rawFree);

        const int8_t rssi = WiFi.RSSI();
        if (rssi < -85)
            LOG_WARN(TAG, "WiFi RSSI lemah: %d dBm", rssi);

        if (!g_mqtt.isWifiConnected())
        {
            static uint32_t wifiDownSince = 0;
            if (wifiDownSince == 0) wifiDownSince = millis();
            if (millis() - wifiDownSince > 30000)
                g_watchdog.triggerRestart("WiFi down lebih dari 30 detik");
        }

        LOG_INFO(TAG,
                 "rawQ=%u/%u | mqttQ=%u/%u (%.0f%%) | WiFi=%s RSSI=%d | heap=%lu KB",
                 rawUsed,  rawUsed  + rawFree,
                 mqttUsed, mqttUsed + mqttFree, mqttPct,
                 g_mqtt.isWifiConnected() ? "OK" : "DOWN",
                 rssi,
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
    LOG_INFO(TAG, "  Health Monitor Mesh — Node %d | Role: %s",
             NODE_ID,
             (NODE_ROLE == ROLE_SENSOR) ? "SENSOR" : "GATEWAY");
    LOG_INFO(TAG, "  Send interval : %lu ms", (unsigned long)Timing::SEND_INTERVAL_MS);
    LOG_INFO(TAG, "  Log level     : %d", LOG_LEVEL);
    LOG_INFO(TAG, "================================================");

    g_watchdog.begin(true);

// ── SENSOR NODE ──────────────────────────────────────────────────────────────
#if NODE_ROLE == ROLE_SENSOR

    g_wire0Mutex = xSemaphoreCreateMutex();
    g_wire1Mutex = xSemaphoreCreateMutex();
    if (!g_wire0Mutex || !g_wire1Mutex)
        g_watchdog.triggerRestart("Gagal buat I2C mutex");

    // (1) IMU — Wire1, tidak bergantung apapun
    if (!g_imu.begin())
        g_watchdog.triggerRestart("MPU6050 init gagal");

    // (2) ESP-NOW — HARUS sebelum Wire (PPG)
    if (!g_mesh.begin(true))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    // (3) PPG — Wire, HARUS setelah ESP-NOW
    if (!g_ppg.begin())
        LOG_WARN(TAG, "MAX30102 gagal — lanjut tanpa PPG (finger detection = false)");

    LOG_INFO(TAG, "Finger gate : %s (threshold IR=%lu)",
             EdgeConfig::ENABLE_FINGER_GATE ? "AKTIF" : "NONAKTIF",
             (unsigned long)EdgeConfig::IR_FINGER_THRESHOLD);

    xTaskCreatePinnedToCore(taskReadPPG,      "PPG",     StackSize::SENSOR_PPG,
                            nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskReadIMU,      "IMU",     StackSize::SENSOR_IMU,
                            nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender,     "CS_TX",   StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX,  nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorSensor,"MONITOR", StackSize::MONITOR,
                            nullptr, 1,                    nullptr, 0);

    LOG_INFO(TAG, "Sensor node siap — 4 task terdaftar");

// ── GATEWAY NODE ─────────────────────────────────────────────────────────────
#elif NODE_ROLE == ROLE_GATEWAY

    // (1) Buat queue SEBELUM mesh.begin() — ISR butuh g_rawQueue
    g_rawQueue = xQueueCreate(10, sizeof(RawPacket));
    if (!g_rawQueue)
        g_watchdog.triggerRestart("Gagal buat g_rawQueue");

    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));
    if (!g_mqttQueue)
        g_watchdog.triggerRestart("Gagal buat g_mqttQueue");

    LOG_DEBUG(TAG, "Queue dibuat | rawQ: 10×%d bytes | mqttQ: %d×%d bytes",
              sizeof(RawPacket), QueueLen::MQTT_MSG, sizeof(MqttMessage));

    // (2) WiFi + MQTT — harus sebelum ESP-NOW agar channel terkunci
    if (!g_mqtt.begin())
        g_watchdog.triggerRestart("WiFi/MQTT init gagal");

    // (3) ESP-NOW — setelah WiFi konek
    if (!g_mesh.begin(false))
        g_watchdog.triggerRestart("ESP-NOW init gagal");

    xTaskCreatePinnedToCore(taskMeshHandler,    "HANDLER", StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB + 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskMqttPublish,    "MQTT",    StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB,     nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorGateway, "MONITOR", StackSize::MONITOR,
                            nullptr, 1,                      nullptr, 0);

    LOG_INFO(TAG, "Gateway siap — 3 task terdaftar");
    LOG_INFO(TAG, "Pipeline: ISR → rawQueue → taskMeshHandler → mqttQueue → taskMqttPublish");

#endif
}


// =============================================================================
// loop() — sengaja kosong, semua kerja di FreeRTOS task
// =============================================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}