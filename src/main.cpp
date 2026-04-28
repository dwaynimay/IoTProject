// =============================================================================
// main.cpp — Entry Point & Orkestrasi FreeRTOS Task
//
// SENSOR NODE (ROLE_SENSOR):
//   Core 1: taskReadPPG  → update() tanpa delay → snapshot ke queue tiap 100ms
//   Core 1: taskReadIMU  → baca tiap 100ms → queue
//   Core 0: taskSendEspNow → ambil queue → kirim ke gateway
//
// GATEWAY NODE (ROLE_GATEWAY):
//   Urutan init WAJIB: WiFi.mode → WiFi.begin (konek dulu!) → esp_now_init
//   [ESP-NOW onDataRecv ISR] → g_mqttQueue → taskMqttPublish
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "DataModels.h"
#include "Sensor_MPU.h"
#include "Sensor_PPG.h"
#include "Network_EspNow.h"
#include "Network_Mqtt.h"

// ===========================================================================
// SENSOR NODE
// ===========================================================================
#if NODE_ROLE == ROLE_SENSOR

static SensorMPU     g_imu;
static SensorPPG     g_ppg;
static NetworkEspNow g_espnow;

static QueueHandle_t g_imuQueue;
static QueueHandle_t g_ppgQueue;

static void taskReadPPG(void* param) {
    PpgPacket pkt{};
    pkt.header.type    = PacketType::PPG_DATA;
    pkt.header.node_id = NODE_ID;
    uint32_t lastSnapshot = 0;

    for (;;) {
        g_ppg.update(); // polling secepat mungkin, tanpa delay

        if (millis() - lastSnapshot >= Timing::PRINT_MS) {
            if (g_ppg.read(pkt.data)) {
                pkt.header.timestamp = millis();
                xQueueOverwrite(g_ppgQueue, &pkt);
            }
            lastSnapshot = millis();
        }
        taskYIELD();
    }
}

static void taskReadIMU(void* param) {
    ImuPacket pkt{};
    pkt.header.type    = PacketType::IMU_DATA;
    pkt.header.node_id = NODE_ID;
    uint32_t lastRead = 0;

    for (;;) {
        if (millis() - lastRead >= Timing::PRINT_MS) {
            if (g_imu.read(pkt.data)) {
                pkt.header.timestamp = millis();
                xQueueOverwrite(g_imuQueue, &pkt);
            }
            lastRead = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void taskSendEspNow(void* param) {
    TickType_t xLastWake = xTaskGetTickCount();
    ImuPacket imuPkt{};
    PpgPacket ppgPkt{};
    uint32_t lastHeartbeat = 0;

    for (;;) {
        if (xQueuePeek(g_imuQueue, &imuPkt, 0) == pdTRUE) {
            g_espnow.sendImu(imuPkt);
        }
        if (xQueuePeek(g_ppgQueue, &ppgPkt, 0) == pdTRUE) {
            g_espnow.sendPpg(ppgPkt);
        }
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
// GATEWAY NODE
// ===========================================================================
#if NODE_ROLE == ROLE_GATEWAY

static NetworkEspNow g_espnow;
static NetworkMqtt   g_mqtt;

static void taskMqttPublish(void* param) {
    MqttMessage msg{};
    for (;;) {
        if (xQueueReceive(g_mqttQueue, &msg, pdMS_TO_TICKS(Timing::MQTT_PUBLISH_MS)) == pdTRUE) {
            g_mqtt.publish(msg.topic, msg.payload);
        }
        g_mqtt.loop();
    }
}

#endif // ROLE_GATEWAY

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- Memulai Health Monitor ---");

// ---------------------------------------------------------------------------
#if NODE_ROLE == ROLE_SENSOR

    // Sensor: MPU dulu (Wire.begin ada di sini), lalu PPG reuse wire
    if (!g_imu.begin()) {
        Serial.println("[FATAL] MPU6050 gagal! Cek SDA=18, SCL=19.");
        while (true) delay(1000);
    }
    if (!g_ppg.begin()) {
        Serial.println("[WARN] MAX30102 gagal. Lanjut tanpa PPG.");
    }

    Serial.println("Tempel jari Anda ke sensor MAX30102...");
    Serial.println("----------------------------------------");

    // ESP-NOW sensor: WiFi.mode → set channel → esp_now_init → add peer
    if (!g_espnow.begin(true)) {
        Serial.println("[FATAL] ESP-NOW gagal!");
        while (true) delay(1000);
    }

    g_imuQueue = xQueueCreate(QueueLen::IMU_DATA, sizeof(ImuPacket));
    g_ppgQueue = xQueueCreate(QueueLen::PPG_DATA, sizeof(PpgPacket));

    xTaskCreatePinnedToCore(taskReadPPG,    "PPG",    StackSize::SENSOR_PPG, nullptr, TaskPrio::SENSOR_PPG, nullptr, 1);
    xTaskCreatePinnedToCore(taskReadIMU,    "IMU",    StackSize::SENSOR_IMU, nullptr, TaskPrio::SENSOR_IMU, nullptr, 1);
    xTaskCreatePinnedToCore(taskSendEspNow, "ESPNOW", StackSize::ESPNOW_TX,  nullptr, TaskPrio::ESPNOW_TX,  nullptr, 0);

    Serial.println("[SETUP] Sensor node siap.");

// ---------------------------------------------------------------------------
#elif NODE_ROLE == ROLE_GATEWAY

    // -----------------------------------------------------------------------
    // URUTAN GATEWAY — SANGAT PENTING, JANGAN DIBALIK:
    //
    //  1. Buat queue DULU (sebelum ESP-NOW init, karena callback ISR
    //     bisa langsung tembak queue begitu esp_now_init selesai)
    //
    //  2. WiFi.mode(STA) + WiFi.begin() → KONEK KE ROUTER DULU
    //     Channel baru terset dengan benar setelah konek
    //
    //  3. BARU esp_now_init() — sekarang channel sudah pasti = channel router
    //
    //  4. Daftarkan peer sensor (channel harus sudah benar)
    // -----------------------------------------------------------------------

    // Step 1: buat queue
    g_mqttQueue = xQueueCreate(QueueLen::MQTT_MSG, sizeof(MqttMessage));

    // Step 2 & 3 & 4: WiFi konek dulu, baru ESP-NOW
    // NetworkMqtt::begin() → connectWifi() → WiFi.begin() + tunggu konek
    // NetworkEspNow::begin(false) → esp_now_init() saat channel sudah benar
    //
    // ⚠️  Urutan ini kebalikan dari sensor node!
    //     Gateway: MQTT.begin() dulu → EspNow.begin() setelah WiFi konek
    if (!g_mqtt.begin()) {
        // Jika WiFi gagal, ESP-NOW juga tidak akan bisa dapat channel yang benar
        Serial.println("[FATAL] WiFi gagal! ESP-NOW channel tidak bisa ditentukan.");
        while (true) delay(1000);
    }

    // Step 4: ESP-NOW init SETELAH WiFi konek (channel sudah benar)
    if (!g_espnow.begin(false)) {
        Serial.println("[FATAL] ESP-NOW gagal!");
        while (true) delay(1000);
    }

    xTaskCreatePinnedToCore(taskMqttPublish, "MQTT", StackSize::MQTT_PUB, nullptr, TaskPrio::MQTT_PUB, nullptr, 0);

    Serial.println("[SETUP] Gateway siap.");

#endif
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}