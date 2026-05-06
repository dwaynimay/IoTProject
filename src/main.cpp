// =============================================================================
// main.cpp — Entry Point & Orkestrasi FreeRTOS Task
//
// SENSOR NODE (ROLE_SENSOR):
//   Core 1: taskReadPPG  → update() tanpa delay → simpan ke state
//   Core 1: taskReadIMU  → baca tiap IMU_SAMPLE_MS → simpan ke state
//   Core 0: taskSendEspNow → setiap SEND_INTERVAL_MS:
//             1. Ambil snapshot IMU + PPG terbaru
//             2. Edge: cek finger_on via IR threshold
//             3. Jika ENABLE_FINGER_GATE && !finger_on → skip kirim
//             4. Bangun CombinedPacket → kirim 1 frame ke gateway
//
// GATEWAY NODE (ROLE_GATEWAY):
//   Init: WiFi konek dulu → esp_now_init → register callback
//   [ESP-NOW onDataRecv ISR] → parse CombinedPacket → format JSON
//   Batching: jika BATCHING_ENABLED, kumpulkan BATCH_SIZE sampel per node
//             lalu push 1 JSON array ke g_mqttQueue
//   [taskMqttPublish] → ambil dari queue → publish ke broker
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

// ---------------------------------------------------------------------------
// FIX: Dua mutex terpisah untuk dua bus I2C yang berbeda.
//
// Sebelumnya: satu g_wireMutex untuk Wire DAN Wire1.
//   → taskReadPPG (prio 4) selalu menang mutex vs taskReadIMU (prio 3)
//   → Wire1 (MPU) nyaris tidak pernah dapat giliran → gyro selalu 0
//
// Sesudah: g_wire0Mutex untuk Wire (MAX30102), g_wire1Mutex untuk Wire1 (MPU).
//   → Kedua bus berjalan paralel dan independen — tidak saling block.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t g_wire0Mutex = nullptr; // Wire  — MAX30102 (pin 18/19)
static SemaphoreHandle_t g_wire1Mutex = nullptr; // Wire1 — MPU6050  (pin 21/22)

// ─── I2C dengan timeout ───────────────────────────────────────────────────────
// Mengganti Wire.endTransmission() langsung
// Kembalikan true jika OK dalam batas waktu
static bool wireTransmitWithTimeout(uint32_t timeoutMs = 100)
{
    uint32_t start = millis();
    uint8_t err = Wire1.endTransmission();
    if (millis() - start > timeoutMs)
    {
        Serial.println("[I2C] WARN: endTransmission timeout!");
        return false;
    }
    return err == 0;
}

// Shared state antara task sensor dan task kirim.
portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;

ImuSample g_latestImu{};
PpgSample g_latestPpg{};
static bool g_imuReady = true;
static bool g_ppgReady = true;

// ---------------------------------------------------------------------------
// taskReadPPG — Core 1, prioritas tertinggi
// ---------------------------------------------------------------------------
static void taskReadPPG(void *param)
{
    g_watchdog.registerTask(); // daftarkan ke HW WDT

    for (;;)
    {
        g_watchdog.feed(); // feed WDT setiap iterasi

        // Gunakan g_wire0Mutex — hanya untuk Wire (MAX30102)
        if (xSemaphoreTake(g_wire0Mutex, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            g_ppg.update();
            xSemaphoreGive(g_wire0Mutex);
        }
        else
        {
            // Mutex tidak bisa diambil dalam 200ms → kemungkinan deadlock
            Serial.println("[PPG] WARN: mutex timeout 200ms");
        }

        // read() hanya baca variabel internal — tidak akses Wire, no mutex needed
        PpgSample snap{};
        g_ppg.read(snap);

        taskENTER_CRITICAL(&g_stateMux);
        g_latestPpg = snap;
        taskEXIT_CRITICAL(&g_stateMux);

        // Stack check setiap 100 iterasi
        static uint32_t iter = 0;
        if (++iter % 100 == 0)
        {
            g_watchdog.checkTaskStack("PPG");
        }

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

            // Gunakan g_wire1Mutex — hanya untuk Wire1 (MPU6050)
            // Karena bus berbeda dengan PPG, tidak perlu tunggu taskReadPPG selesai
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
                {
                    // 50 kegagalan berturut-turut → I2C mungkin hang
                    g_watchdog.triggerRestart("IMU read fail 50x");
                }
            }
            lastRead = now;
        }

        static uint32_t iter = 0;
        if (++iter % 200 == 0)
        {
            g_watchdog.checkTaskStack("IMU");
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// taskSendEspNow — Core 0
// Setiap SEND_INTERVAL_MS: snapshot → edge check → build CombinedPacket → kirim
// ---------------------------------------------------------------------------
static void taskSendEspNow(void *param)
{
    g_watchdog.registerTask();

    TickType_t xLastWake = xTaskGetTickCount();
    uint32_t lastHeartbeat = 0;
    uint32_t lastDebugLog = 0;
    uint32_t sentCount = 0;
    uint32_t nackCount = 0;

    // Tunggu 1 detik sebelum mulai kirim — beri waktu task sensor populate data
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("[ESPNOW] taskSendEspNow dimulai, mulai kirim CombinedPacket...");

    for (;;)
    {
        g_watchdog.feed();

        // === 1. Snapshot ===
        ImuSample imuSnap{};
        PpgSample ppgSnap{};

        taskENTER_CRITICAL(&g_stateMux);
        imuSnap = g_latestImu;
        ppgSnap = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        // === 2. Edge: finger-on detection ===
        EdgeResult edge{};
        edge.finger_on = (ppgSnap.ir_raw >= EdgeConfig::IR_FINGER_THRESHOLD);
        edge.reserved = 0;

        // === 3. Gate ===
        bool shouldSend = true;
        if (EdgeConfig::ENABLE_FINGER_GATE && !edge.finger_on)
        {
            shouldSend = false;
        }

        // === 4. Debug log setiap 2 detik (selalu cetak, gate atau tidak) ===
        if (millis() - lastDebugLog >= 2000)
        {
            Serial.printf("[DBG] hasIMU=1 hasPPG=1 | IR=%lu | finger=%s | gate=%s | shouldSend=%s | sent=%lu nack=%lu\n",
                          (unsigned long)ppgSnap.ir_raw,
                          edge.finger_on ? "YES" : "NO",
                          EdgeConfig::ENABLE_FINGER_GATE ? "ON" : "OFF",
                          shouldSend ? "YES" : "NO",
                          sentCount, nackCount);
            Serial.printf("[DBG] IMU → ax=%.4f ay=%.4f az=%.4f | gx=%.4f gy=%.4f gz=%.4f\n",
                          imuSnap.accel_x, imuSnap.accel_y, imuSnap.accel_z,
                          imuSnap.gyro_x, imuSnap.gyro_y, imuSnap.gyro_z);
            lastDebugLog = millis();
        }

        if (shouldSend)
        {
            // === 5. Bangun CombinedPacket ===
            CombinedPacket pkt{};
            pkt.header.type = PacketType::COMBINED_DATA;
            pkt.header.node_id = NODE_ID;
            pkt.header.timestamp = millis();
            pkt.imu = imuSnap;
            pkt.ppg = ppgSnap;
            pkt.edge = edge;

            // === 6. Kirim ===
            bool ok = g_espnow.sendCombined(pkt);
            if (ok)
            {
                sentCount++;
                Serial.printf("[TX] #%lu Node %d | IR=%lu finger=%s | HR=%d | ax=%.2f ay=%.2f az=%.2f | gx=%.2f gy=%.2f gz=%.2f\n",
                              sentCount, NODE_ID,
                              (unsigned long)ppgSnap.ir_raw,
                              edge.finger_on ? "Y" : "N",
                              ppgSnap.heart_rate,
                              imuSnap.accel_x, imuSnap.accel_y, imuSnap.accel_z,
                              imuSnap.gyro_x, imuSnap.gyro_y, imuSnap.gyro_z);
            }
            else
            {
                nackCount++;
                Serial.printf("[TX] GAGAL #%lu (esp_now_send error)\n", nackCount);
            }
        }

        // === Heartbeat periodik ===
        if (millis() - lastHeartbeat >= Timing::HEARTBEAT_MS)
        {
            HeartbeatPacket hb{};
            hb.header.type = PacketType::HEARTBEAT;
            hb.header.node_id = NODE_ID;
            hb.header.timestamp = millis();
            hb.uptime_s = millis() / 1000;
            hb.rssi = 0;
            g_espnow.sendHeartbeat(hb);
            lastHeartbeat = millis();
            Serial.printf("[HB] Heartbeat dikirim (uptime=%lus)\n", hb.uptime_s);
        }

        // === Throttle — tunggu sampai interval berikutnya ===
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::SEND_INTERVAL_MS));

        static uint32_t iter = 0;
        if (++iter % 500 == 0)
        {
            g_watchdog.checkTaskStack("ESPNOW_TX");
        }
    }
}

// ─── taskMonitor — health check periodik ──────────────────────────────────────
static void taskMonitor(void *param)
{
    // Monitor TIDAK didaftarkan ke HW WDT karena tugasnya justru memantau
    // Jika monitor sendiri macet, HW WDT idle task akan catch-nya
    for (;;)
    {
        g_watchdog.healthCheck(); // print stats + cek heap

        // Cek stack semua task utama
        g_watchdog.checkTaskStack("PPG");
        g_watchdog.checkTaskStack("IMU");
        g_watchdog.checkTaskStack("ESPNOW_TX");

        // Cek minimum free heap — jika < 20KB, restart preventif
        if (esp_get_free_heap_size() < 20 * 1024)
        {
            g_watchdog.triggerRestart("Heap < 20KB");
        }

        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_MS));
    }
}

#endif // ROLE_SENSOR

// ===========================================================================
// GATEWAY NODE
// ===========================================================================
#if NODE_ROLE == ROLE_GATEWAY

static NetworkEspNow g_espnow;
static NetworkMqtt g_mqtt;

// ---------------------------------------------------------------------------
// taskMqttPublish — Core 0
// ---------------------------------------------------------------------------
static void taskMqttPublish(void *param)
{
    g_watchdog.registerTask();

    MqttMessage msg{};
    uint32_t publishedCount = 0;
    uint32_t failCount = 0;
    uint32_t lastStatusLog = 0;

    for (;;)
    {
        g_watchdog.feed();

        if (millis() - lastStatusLog >= 10000)
        {
            Serial.printf("[MQTT] Status: WiFi=%s MQTT=%s | published=%lu fail=%lu queue=%u\n",
                          g_mqtt.isWifiConnected() ? "OK" : "DOWN",
                          g_mqtt.isConnected() ? "OK" : "DOWN",
                          publishedCount, failCount,
                          uxQueueMessagesWaiting(g_mqttQueue));
            lastStatusLog = millis();
        }

        if (!g_mqtt.isConnected())
        {
            g_mqtt.loop();
            // Buang 1 pesan dari queue agar tidak overflow saat offline
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
                // 10 kegagalan berurut → MQTT connection mungkin zombie
                if (failCount > 10 && publishedCount == 0)
                {
                    g_watchdog.triggerRestart("MQTT publish fail 10x");
                }
                Serial.printf("[MQTT] GAGAL #%lu → %s (rc=%d)\n",
                              failCount, msg.topic, g_mqtt.state());
            }
        }

        g_mqtt.loop();

        static uint32_t iter = 0;
        if (++iter % 100 == 0)
        {
            g_watchdog.checkTaskStack("MQTT_PUB");
        }
    }
}

// ─── taskMonitor Gateway ──────────────────────────────────────────────────────
static void taskMonitorGateway(void *param)
{
    for (;;)
    {
        g_watchdog.healthCheck();

        // Cek queue fill level — kalau penuh terus, MQTT lambat
        UBaseType_t queueUsed = uxQueueMessagesWaiting(g_mqttQueue);
        UBaseType_t queueFree = uxQueueSpacesAvailable(g_mqttQueue);
        UBaseType_t queueTotal = queueUsed + queueFree;
        float queueFillPct = 100.0f * queueUsed / queueTotal;

        if (queueFillPct > 80.0f)
        {
            Serial.printf("[MONITOR] ⚠ Queue MQTT: %.0f%% penuh (%d/%d)\n",
                          queueFillPct, queueUsed, queueTotal);
        }

        // Cek WiFi RSSI
        int8_t rssi = WiFi.RSSI();
        if (rssi < -85)
        {
            Serial.printf("[MONITOR] ⚠ WiFi RSSI lemah: %d dBm\n", rssi);
        }

        // Cek WiFi status
        if (WiFi.status() != WL_CONNECTED)
        {
            static uint32_t wifiFailSince = 0;
            if (wifiFailSince == 0)
                wifiFailSince = millis();
            if (millis() - wifiFailSince > 30000)
            {
                // WiFi mati > 30 detik → restart
                g_watchdog.triggerRestart("WiFi down 30s");
            }
        }
        else
        {
            // Reset counter jika WiFi kembali
            // (tidak bisa akses static var langsung, tidak masalah)
        }

        Serial.printf("[MONITOR] Queue: %d/%d (%.0f%%) | "
                      "WiFi: %s RSSI=%d | Heap: %luKB\n",
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
    Serial.println("\n=== Health Monitor (CombinedPacket + EdgeComputing) ===");
    Serial.printf("Node ID : %d\n", NODE_ID);
    Serial.printf("Role    : %s\n", (NODE_ROLE == ROLE_SENSOR) ? "SENSOR" : "GATEWAY");
    Serial.printf("Interval: %lu ms (%s)\n",
                  (unsigned long)Timing::SEND_INTERVAL_MS,
                  (NODE_ROLE == ROLE_SENSOR) ? "kirim CombinedPacket" : "MQTT publish timeout");
    Serial.println("=====================================================\n");

    // ---------------------------------------------------------------------------
    // Watchdog harus di-init SEBELUM task dibuat
    g_watchdog.begin(true);

// ---------------------------------------------------------------------------
#if NODE_ROLE == ROLE_SENSOR

    // 1. Buat DUA mutex terpisah untuk Wire dan Wire1
    g_wire0Mutex = xSemaphoreCreateMutex(); // Wire  — MAX30102
    g_wire1Mutex = xSemaphoreCreateMutex(); // Wire1 — MPU6050
    if (!g_wire0Mutex || !g_wire1Mutex)
    {
        g_watchdog.triggerRestart("Mutex create fail");
    }

    // 2. Init MPU6050 via Wire1 (pin SDA=21, SCL=22)
    if (!g_imu.begin())
    {
        g_watchdog.triggerRestart("MPU6050 init fail");
    }
    // g_imu.calibrate(500); // uncomment untuk reset offset kalibrasi

    Serial.printf("[CONFIG] Finger gate  : %s (threshold IR=%lu)\n",
                  EdgeConfig::ENABLE_FINGER_GATE ? "AKTIF" : "NONAKTIF",
                  (unsigned long)EdgeConfig::IR_FINGER_THRESHOLD);
    Serial.printf("[CONFIG] Send interval: %lu ms\n",
                  (unsigned long)Timing::SEND_INTERVAL_MS);

    // 3. ESP-NOW DULU — WAJIB sebelum Wire.begin() untuk MAX30102
    if (!g_espnow.begin(true))
    {
        g_watchdog.triggerRestart("ESP-NOW init fail");
    }

    // 4. MAX30102 via Wire (pin SDA=18, SCL=19) — SETELAH esp_now_init
    if (!g_ppg.begin())
    {
        Serial.println("[WARN] MAX30102 gagal. Lanjut tanpa PPG.");
        Serial.println("[WARN] Finger detection akan selalu false.");
    }

    // 5. Spawn FreeRTOS tasks
    xTaskCreatePinnedToCore(taskReadPPG, "PPG", StackSize::SENSOR_PPG,
                            nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskReadIMU, "IMU", StackSize::SENSOR_IMU,
                            nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskCSSender, "CS_TX", StackSize::ESPNOW_TX,
                            nullptr, TaskPrio::ESPNOW_TX, nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitor, "MONITOR", 3072,
                            nullptr, 1, nullptr, 0);

    Serial.println("[SETUP] Sensor node siap.");
    Serial.println("[SETUP] Tempel jari ke MAX30102 untuk mulai kirim data...");

// ---------------------------------------------------------------------------
#elif NODE_ROLE == ROLE_GATEWAY

    Serial.printf("[CONFIG] Batching: %s",
                  BatchConfig::BATCHING_ENABLED ? "AKTIF" : "NONAKTIF");
    if (BatchConfig::BATCHING_ENABLED)
    {
        Serial.printf(" (size=%d, latensi tambah ~%lu ms)",
                      BatchConfig::BATCH_SIZE,
                      (unsigned long)(BatchConfig::BATCH_SIZE * Timing::SEND_INTERVAL_MS));
    }
    Serial.println();

    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));
    if (!g_mqttQueue)
    {
        g_watchdog.triggerRestart("Queue create fail");
    }
    Serial.printf("[SETUP] MQTT queue dibuat (size=%d × %d bytes)\n",
                  QueueLen::MQTT_MSG, sizeof(MqttMessage));

    if (!g_mqtt.begin())
    {
        g_watchdog.triggerRestart("WiFi/MQTT init fail");
    }

    if (!g_espnow.begin(false))
    {
        g_watchdog.triggerRestart("ESP-NOW init fail");
    }

    xTaskCreatePinnedToCore(taskMqttPublish, "MQTT", StackSize::MQTT_PUB,
                            nullptr, TaskPrio::MQTT_PUB, nullptr, 0);
    xTaskCreatePinnedToCore(taskMonitorGateway, "MONITOR", 3072,
                            nullptr, 1, nullptr, 0);

    Serial.println("[SETUP] Gateway siap.");
    Serial.println("[SETUP] Menunggu data dari sensor node...");

#endif
}

// loop() tidak dipakai — semua ada di FreeRTOS task
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
}