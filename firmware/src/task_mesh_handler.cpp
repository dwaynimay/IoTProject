// File: src/task_mesh_handler.cpp

// =============================================================================
// task_mesh_handler.cpp — Task Handler untuk Gateway Node
// =============================================================================
//
// File ini berisi dua FreeRTOS task yang bekerja bersama di gateway:
//
//   taskMeshHandler   (Core 1, prioritas lebih tinggi)
//     Ambil RawPacket dari g_rawQueue → MeshRouting::route() → push g_mqttQueue
//     Ini adalah "otak" gateway: decode packet dan tentukan topic MQTT.
//
//   taskMqttPublish   (Core 0, prioritas normal)
//     Ambil MqttMessage dari g_mqttQueue → g_mqtt.publish()
//     Juga handle reconnect MQTT via tryReconnect().
//
// Kenapa dipisah dari main.cpp?
//   main.cpp seharusnya hanya orkestrator (setup + task registration).
//   Logika task yang panjang di main.cpp membuat file sulit dibaca dan
//   sulit di-test secara independen.
//
// Dependency:
//   - EspNowMesh  : g_rawQueue, g_mqttQueue (extern)
//   - MeshRouting : route()
//   - Network_Mqtt: g_mqtt (extern)
//   - Watchdog    : g_watchdog (extern)
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "DataModels.h"
#include "EspNowMesh/MeshPackets.h"
#include "EspNowMesh/MeshRouting.h"
#include "Network_Mqtt/Network_Mqtt.h"
#include "Watchdog/Watchdog.h"

// Referensi ke instance global yang didefinisikan di main.cpp
extern NetworkMqtt g_mqtt;

static constexpr char TAG_HANDLER[] = "HANDLER";
static constexpr char TAG_PUBLISH[] = "PUBLISH";


// =============================================================================
// taskMeshHandler — Core 1
// =============================================================================
//
// Pipeline per iterasi:
//   1. Block di xQueueReceive(g_rawQueue) — hemat CPU saat tidak ada data
//   2. Panggil MeshRouting::route() untuk decode + serialize ke JSON
//   3. Push MqttMessage ke g_mqttQueue
//
// Jika g_mqttQueue penuh, paket dibuang dan drop counter dinaikkan.
// Drop rate bisa dipantau via log periodik setiap 10 detik.
// =============================================================================
void taskMeshHandler(void* param)
{
    g_watchdog.registerTask();

    RawPacket   raw{};
    MqttMessage msg{};

    uint32_t receivedCount = 0;
    uint32_t droppedCount  = 0;
    uint32_t lastLogMs     = 0;

    LOG_INFO(TAG_HANDLER, "taskMeshHandler dimulai (Core 1)");

    for (;;)
    {
        g_watchdog.feed();

        // ── Tunggu packet dari ISR ────────────────────────────────────────────
        // Timeout 500ms: jika tidak ada data, kembali ke atas untuk feed WDT
        if (xQueueReceive(g_rawQueue, &raw, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        receivedCount++;

        // ── Route packet ke MqttMessage ───────────────────────────────────────
        if (!MeshRouting::route(raw, msg))
        {
            // route() sudah LOG_WARN untuk packet tidak dikenal
            droppedCount++;
            continue;
        }

        // ── Push ke MQTT queue ────────────────────────────────────────────────
        // Tidak blocking (timeout=0): lebih baik drop satu paket daripada
        // block task ini dan biarkan g_rawQueue overflow di sisi ISR.
        if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
        {
            droppedCount++;
            LOG_EVERY_N(20, LOG_WARN, TAG_HANDLER,
                        "g_mqttQueue penuh — paket dibuang (total drop=%lu)",
                        droppedCount);
        }

        // ── Log throughput setiap 10 detik ───────────────────────────────────
        if (millis() - lastLogMs >= 10000)
        {
            lastLogMs = millis();
            LOG_INFO(TAG_HANDLER,
                     "Throughput | recv=%lu drop=%lu | rawQ=%u mqttQ=%u",
                     receivedCount, droppedCount,
                     uxQueueMessagesWaiting(g_rawQueue),
                     uxQueueMessagesWaiting(g_mqttQueue));
        }

        // Stack check setiap 500 iterasi
        LOG_EVERY_N(500, LOG_DEBUG, TAG_HANDLER,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));
    }
}


// =============================================================================
// taskMqttPublish — Core 0
// =============================================================================
//
// Pipeline per iterasi:
//   1. Cek koneksi MQTT — jika putus, coba reconnect via tryReconnect()
//   2. Block di xQueueReceive(g_mqttQueue) dengan timeout MQTT_PUBLISH_MS
//   3. Publish ke broker via g_mqtt.publish()
//   4. Panggil g_mqtt.loop() untuk jaga keepalive
//
// Reconnect tidak blocking — tryReconnect() menggunakan exponential backoff
// sehingga task ini tidak freeze saat broker sedang down.
// =============================================================================
void taskMqttPublish(void* param)
{
    g_watchdog.registerTask();

    MqttMessage msg{};
    uint32_t    lastStatusLogMs = 0;

    LOG_INFO(TAG_PUBLISH, "taskMqttPublish dimulai (Core 0)");

    for (;;)
    {
        g_watchdog.feed();

        // ── Status log setiap 10 detik ────────────────────────────────────────
        if (millis() - lastStatusLogMs >= 10000)
        {
            lastStatusLogMs = millis();
            LOG_INFO(TAG_PUBLISH,
                     "Status | WiFi=%s MQTT=%s | pub=%lu fail=%lu | queue=%u",
                     g_mqtt.isWifiConnected() ? "OK"   : "DOWN",
                     g_mqtt.isConnected()     ? "OK"   : "DOWN",
                     g_mqtt.publishCount(),
                     g_mqtt.failCount(),
                     uxQueueMessagesWaiting(g_mqttQueue));
        }

        // ── Handle koneksi putus ──────────────────────────────────────────────
        if (!g_mqtt.isConnected())
        {
            g_mqtt.tryReconnect(); // non-blocking, pakai exponential backoff

            // Buang antrian saat offline agar tidak stale saat reconnect
            // Hanya buang jika queue > 50% penuh untuk hindari data loss berlebihan
            if (uxQueueMessagesWaiting(g_mqttQueue) > QueueLen::MQTT_MSG / 2)
            {
                xQueueReceive(g_mqttQueue, &msg, 0);
                LOG_EVERY_N(5, LOG_WARN, TAG_PUBLISH,
                            "MQTT offline — buang 1 pesan dari antrian");
            }

            g_mqtt.loop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Ambil dan publish pesan ───────────────────────────────────────────
        if (xQueueReceive(g_mqttQueue, &msg,
                          pdMS_TO_TICKS(Timing::MQTT_PUBLISH_MS)) == pdTRUE)
        {
            g_mqtt.publish(msg.topic, msg.payload);
            // publish() sudah handle logging sukses/gagal di dalam Network_Mqtt
        }

        g_mqtt.loop();

        // Stack check setiap 100 iterasi
        LOG_EVERY_N(100, LOG_DEBUG, TAG_PUBLISH,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));
    }
}