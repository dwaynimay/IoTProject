// =============================================================================
// main.cpp — Entry Point & Orkestrasi FreeRTOS Task
//
// Node dikonfigurasi via NODE_ROLE di platformio.ini:
//   ROLE_SENSOR  → baca sensor, kirim via ESP-NOW ke gateway
//   ROLE_GATEWAY → terima ESP-NOW, publish ke MQTT
//
// Arsitektur Task:
// ┌─────────────────────────────────────────────────────────────────┐
// │  SENSOR NODE                                                    │
// │  taskReadImu ──► imuQueue ──► taskSendEspNow ──► [ESP-NOW] ──► │
// │  taskReadPpg ──► ppgQueue ──┘                                   │
// └─────────────────────────────────────────────────────────────────┘
// ┌─────────────────────────────────────────────────────────────────┐
// │  GATEWAY NODE                                                   │
// │  [ESP-NOW] ──► onDataRecv (ISR) ──► g_mqttQueue ──► taskMqttPub│
// └─────────────────────────────────────────────────────────────────┘
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "DataModels.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "Network_EspNow.h"
#include "Network_Mqtt.h"

// ---------------------------------------------------------------------------
// Objek global
// ---------------------------------------------------------------------------
#if NODE_ROLE == ROLE_SENSOR
    static SensorMPU     g_imu;
    static SensorPPG     g_ppg;
    static NetworkEspNow g_espnow;

    // Queue antar task sensor → task pengirim
    static QueueHandle_t g_imuQueue;
    static QueueHandle_t g_ppgQueue;
#endif

#if NODE_ROLE == ROLE_GATEWAY
    static NetworkEspNow g_espnow;
    static NetworkMqtt   g_mqtt;
    // g_mqttQueue dideklarasikan extern di Network_EspNow.h
#endif

// ===========================================================================
// ██████╗  ██████╗ ██╗     ███████╗    ███████╗███████╗███╗   ██╗███████╗ ██████╗ ██████╗
// ROLE: SENSOR NODE
// ===========================================================================
#if NODE_ROLE == ROLE_SENSOR

// ---------------------------------------------------------------------------
// Task: Baca IMU 100 Hz
// ---------------------------------------------------------------------------
static void taskReadImu(void* param) {
    TickType_t xLastWake = xTaskGetTickCount();
    ImuPacket pkt{};
    pkt.header.type    = PacketType::IMU_DATA;
    pkt.header.node_id = NODE_ID;

    for (;;) {
        if (g_imu.read(pkt.data)) {
            pkt.header.timestamp = millis();
            xQueueOverwrite(g_imuQueue, &pkt);  // selalu simpan yang terbaru
        }
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS));
    }
}

// ---------------------------------------------------------------------------
// Task: Baca PPG 50 Hz
// ---------------------------------------------------------------------------
static void taskReadPpg(void* param) {
    TickType_t xLastWake = xTaskGetTickCount();
    PpgPacket pkt{};
    pkt.header.type    = PacketType::PPG_DATA;
    pkt.header.node_id = NODE_ID;

    for (;;) {
        g_ppg.update();  // update FIFO internal

        if (g_ppg.read(pkt.data)) {
            pkt.header.timestamp = millis();
            xQueueOverwrite(g_ppgQueue, &pkt);
        }
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::PPG_SAMPLE_MS));
    }
}

// ---------------------------------------------------------------------------
// Task: Kirim data via ESP-NOW ke Gateway (100ms interval)
// ---------------------------------------------------------------------------
static void taskSendEspNow(void* param) {
    TickType_t xLastWake = xTaskGetTickCount();
    ImuPacket imuPkt{};
    PpgPacket ppgPkt{};

    uint32_t lastHeartbeat = 0;

    for (;;) {
        // Kirim IMU jika ada data baru
        if (xQueuePeek(g_imuQueue, &imuPkt, 0) == pdTRUE) {
            g_espnow.sendImu(imuPkt);
        }

        // Kirim PPG jika ada data baru
        if (xQueuePeek(g_ppgQueue, &ppgPkt, 0) == pdTRUE) {
            g_espnow.sendPpg(ppgPkt);
        }

        // Heartbeat setiap 30 detik
        if (millis() - lastHeartbeat > 30000) {
            HeartbeatPacket hb{};
            hb.header.type      = PacketType::HEARTBEAT;
            hb.header.node_id   = NODE_ID;
            hb.header.timestamp = millis();
            hb.uptime_s         = millis() / 1000;
            g_espnow.sendHeartbeat(hb);
            lastHeartbeat = millis();
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Timing::ESPNOW_SEND_MS));
    }
}

#endif // ROLE_SENSOR

// ===========================================================================
// ROLE: GATEWAY NODE
// ===========================================================================
#if NODE_ROLE == ROLE_GATEWAY

// ---------------------------------------------------------------------------
// Task: Baca queue dan publish ke MQTT
// ---------------------------------------------------------------------------
static void taskMqttPublish(void* param) {
    MqttMessage msg{};

    for (;;) {
        // Blokir sampai ada pesan di queue (max 500ms)
        if (xQueueReceive(g_mqttQueue, &msg, pdMS_TO_TICKS(Timing::MQTT_PUBLISH_MS)) == pdTRUE) {
            g_mqtt.publish(msg.topic, msg.payload);
        }

        g_mqtt.loop(); // keepalive MQTT
    }
}

#endif // ROLE_GATEWAY

// ===========================================================================
// setup() & loop()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("==============================================");
    Serial.printf("  ESP32 Health Monitor | Node ID: %d | Role: %s\n",
                  NODE_ID,
                  (NODE_ROLE == ROLE_SENSOR) ? "SENSOR" : "GATEWAY");
    Serial.println("==============================================");

#if NODE_ROLE == ROLE_SENSOR
    // --- Init Sensor ---
    if (!g_imu.begin()) {
        Serial.println("[FATAL] MPU6050 gagal! Halt.");
        while (true) delay(1000);
    }
    g_imu.calibrate();

    if (!g_ppg.begin()) {
        Serial.println("[WARN] MAX30102 gagal! PPG dinonaktifkan.");
        // Tidak halt — sistem bisa jalan tanpa PPG
    }

    // --- Init ESP-NOW (mode sender) ---
    if (!g_espnow.begin(true)) {
        Serial.println("[FATAL] ESP-NOW gagal! Halt.");
        while (true) delay(1000);
    }

    // --- Buat Queue (ukuran 1 — overwrite, selalu data terbaru) ---
    g_imuQueue = xQueueCreate(1, sizeof(ImuPacket));
    g_ppgQueue = xQueueCreate(1, sizeof(PpgPacket));

    // --- Buat FreeRTOS Task ---
    xTaskCreatePinnedToCore(taskReadImu,    "IMU",     StackSize::SENSOR_IMU,  nullptr, TaskPrio::SENSOR_IMU,  nullptr, 1);
    xTaskCreatePinnedToCore(taskReadPpg,    "PPG",     StackSize::SENSOR_PPG,  nullptr, TaskPrio::SENSOR_PPG,  nullptr, 1);
    xTaskCreatePinnedToCore(taskSendEspNow, "ESPNOW",  StackSize::ESPNOW_TX,   nullptr, TaskPrio::ESPNOW_TX,   nullptr, 0);

    Serial.println("[SETUP] Sensor node siap.");
#endif

#if NODE_ROLE == ROLE_GATEWAY
    // --- Buat MQTT Queue (diakses oleh ESP-NOW callback ISR) ---
    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));

    // --- Init ESP-NOW (mode receiver) ---
    if (!g_espnow.begin(false)) {
        Serial.println("[FATAL] ESP-NOW gagal! Halt.");
        while (true) delay(1000);
    }

    // --- Init MQTT (blokir sampai konek) ---
    if (!g_mqtt.begin()) {
        Serial.println("[WARN] MQTT gagal tersambung, akan retry otomatis.");
    }

    // --- Buat FreeRTOS Task ---
    xTaskCreatePinnedToCore(taskMqttPublish, "MQTT_PUB", StackSize::MQTT_PUB, nullptr, TaskPrio::MQTT_PUB, nullptr, 0);

    Serial.println("[SETUP] Gateway node siap.");
#endif
}

// loop() kosong — semua logika ada di FreeRTOS task
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}