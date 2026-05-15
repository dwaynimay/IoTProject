// File: src/main.cpp

// =============================================================================
// main.cpp — Orkestrator: Inisialisasi & Registrasi FreeRTOS Task
// =============================================================================
//
// ATURAN FILE INI:
//   ✓ Deklarasi instance global (sensor, network, dsb.)
//   ✓ setup(): inisialisasi semua modul, buat queue, registrasi task
//   ✓ loop(): hanya vTaskDelay — semua kerja ada di task
//   ✗ JANGAN taruh logika task di sini → task_cs_sender.cpp / task_mesh_handler.cpp
//   ✗ JANGAN taruh logika bisnis di sini → modul di lib/
//
// URUTAN INISIALISASI (penting, jangan diubah):
//   Sensor:
//     1. IMU (Wire1)       → tidak bergantung apapun
//     2. ESP-NOW           → harus sebelum Wire (PPG)
//     3. PPG (Wire)        → harus setelah ESP-NOW
//
//   Gateway:
//     1. Buat queue        → harus sebelum mesh.begin() (ISR butuh g_rawQueue)
//     2. MQTT + WiFi       → harus sebelum esp_now_init
//     3. ESP-NOW           → setelah WiFi konek
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "DataModels.h"

// Modul dari lib/
#include "Watchdog/Watchdog.h"
#include "HealthSensors/Sensor_MPU.h"
#include "HealthSensors/Sensor_PPG.h"
#include "EspNowMesh/EspNowMesh.h"
#include "Network_Mqtt/Network_Mqtt.h"

// Deklarasi task functions (definisi ada di src/ masing-masing)
extern void taskCSSender    (void* param);
extern void taskMeshHandler (void* param);
extern void taskMqttPublish (void* param);

static constexpr char TAG[] = "MAIN";


// =============================================================================
// Instance & Shared State Global
//
// Semua variabel di bawah ini di-extern di task file yang membutuhkannya.
// Pisah deklarasi (di sini) dari penggunaan (di task file) agar tidak ada
// coupling tersembunyi antar file.
// =============================================================================

// ── Shared antara sensor tasks ────────────────────────────────────────────────
// g_stateMux  : critical section untuk baca/tulis g_latestImu & g_latestPpg
// g_latestImu : snapshot IMU terbaru (diupdate taskReadIMU, dibaca taskCSSender)
// g_latestPpg : snapshot PPG terbaru (diupdate taskReadPPG, dibaca taskCSSender)
portMUX_TYPE g_stateMux  = portMUX_INITIALIZER_UNLOCKED;
ImuSample    g_latestImu = {};
PpgSample    g_latestPpg = {};

// ── Mutex untuk bus I2C ───────────────────────────────────────────────────────
// Wire  (bus 1, pin 18/19) → MAX30102 → g_wire0Mutex
// Wire1 (bus 2, pin 21/22) → MPU6050  → g_wire1Mutex
SemaphoreHandle_t g_wire0Mutex = nullptr;
SemaphoreHandle_t g_wire1Mutex = nullptr;

// ── Instance modul ────────────────────────────────────────────────────────────
#if NODE_ROLE == ROLE_SENSOR
    static SensorMPU   g_imu;
    static SensorPPG   g_ppg;
    EspNowMesh         g_mesh; // extern di task_cs_sender.cpp
#endif

#if NODE_ROLE == ROLE_GATEWAY
    static EspNowMesh  g_mesh;
    NetworkMqtt        g_mqtt; // extern di task_mesh_handler.cpp
#endif


// =============================================================================
// SENSOR NODE — Task Definitions
// =============================================================================
#if NODE_ROLE == ROLE_SENSOR

// ---------------------------------------------------------------------------
// taskReadPPG — Core 1, prioritas tertinggi
// Alasan prioritas tinggi: polling PPG harus secepat mungkin agar
// algoritma checkForBeat() tidak kehilangan beat.
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
// taskSendEspNow — Core 0 (mode non-CS: kirim CombinedPacket)
// Hanya aktif jika tidak menggunakan CS sender.
// Untuk mode CS, gunakan taskCSSender.
// ---------------------------------------------------------------------------
static void taskSendEspNow(void* param)
{
    g_watchdog.registerTask();

    TickType_t xLastWake   = xTaskGetTickCount();
    uint32_t   lastHbMs    = 0;

    vTaskDelay(pdMS_TO_TICKS(1000)); // tunggu sensor stabil
    LOG_INFO(TAG, "taskSendEspNow dimulai");

    for (;;)
    {
        g_watchdog.feed();

        ImuSample imu{};
        PpgSample ppg{};
        taskENTER_CRITICAL(&g_stateMux);
        imu = g_latestImu;
        ppg = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        const bool fingerOn = (ppg.irRaw >= EdgeConfig::IR_FINGER_THRESHOLD);
        const bool shouldSend = !EdgeConfig::ENABLE_FINGER_GATE || fingerOn;

        if (shouldSend)
        {
            CombinedPacket pkt{};
            pkt.header  = { PacketType::COMBINED_DATA, NODE_ID,
                            static_cast<uint32_t>(millis()) };
            pkt.imu     = imu;
            pkt.ppg     = ppg;
            pkt.edge    = { fingerOn, 0 };

            g_mesh.sendCombined(pkt);

            LOG_EVERY_N(10, LOG_DEBUG, TAG,
                        "TX CombinedPacket | IR=%lu finger=%s HR=%d",
                        (unsigned long)ppg.irRaw,
                        fingerOn ? "Y" : "N",
                        ppg.heartRate);
        }

        // Heartbeat periodik
        if (millis() - lastHbMs >= Timing::HEARTBEAT_MS)
        {
            g_mesh.sendHeartbeat(NODE_ID, millis() / 1000);
            lastHbMs = millis();
            LOG_DEBUG(TAG, "Heartbeat dikirim | uptime=%lu s", millis() / 1000);
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::SEND_INTERVAL_MS));
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

        // Pantau fill level kedua queue
        const UBaseType_t rawUsed  = uxQueueMessagesWaiting(g_rawQueue);
        const UBaseType_t rawFree  = uxQueueSpacesAvailable(g_rawQueue);
        const UBaseType_t mqttUsed = uxQueueMessagesWaiting(g_mqttQueue);
        const UBaseType_t mqttFree = uxQueueSpacesAvailable(g_mqttQueue);
        const float       mqttPct  = 100.0f * mqttUsed / (mqttUsed + mqttFree);

        if (mqttPct > 80.0f)
        {
            LOG_WARN(TAG, "g_mqttQueue %.0f%% penuh (%u/%u)",
                     mqttPct, mqttUsed, mqttUsed + mqttFree);
        }

        if (rawUsed > 5)
        {
            LOG_WARN(TAG, "g_rawQueue menumpuk: %u/%u",
                     rawUsed, rawUsed + rawFree);
        }

        // WiFi RSSI
        const int8_t rssi = WiFi.RSSI();
        if (rssi < -85)
            LOG_WARN(TAG, "WiFi RSSI lemah: %d dBm", rssi);

        // WiFi down timeout → restart
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
// setup() — Inisialisasi & Registrasi Task
// =============================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    // Banner startup
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

    // Buat mutex bus I2C
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

    // Registrasi FreeRTOS task
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

    LOG_INFO(TAG, "Batching: %s%s",
             BatchConfig::BATCHING_ENABLED ? "AKTIF" : "NONAKTIF",
             BatchConfig::BATCHING_ENABLED
                 ? " (belum diimplementasikan di MeshRouting)"
                 : "");

    // Registrasi FreeRTOS task
    // taskMeshHandler di Core 1, prioritas lebih tinggi dari taskMqttPublish
    // agar g_rawQueue tidak overflow saat broker lambat
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
// loop() — Sengaja dikosongkan
//
// Semua kerja ada di FreeRTOS task. loop() hanya tidur agar idle task
// ESP32 bisa berjalan dan feed hardware WDT.
// =============================================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}