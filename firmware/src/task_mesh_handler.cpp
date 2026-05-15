// File: firmware/src/task_mesh_handler.cpp

// =============================================================================
// task_mesh_handler.cpp — Task Handler untuk Gateway Node
// =============================================================================
//
// PERUBAHAN v2 (refactor):
//   - g_mqttQueue DIDEFINISIKAN di sini (bukan di EspNowMesh.cpp).
//     Ini adalah layer app (routing + MQTT), bukan transport.
//   - g_rawQueue di-extern dari EspNowMesh.cpp (transport layer).
//
// KEPEMILIKAN QUEUE:
//   g_rawQueue  → extern dari EspNowMesh.cpp
//   g_mqttQueue → didefinisikan di sini, di-extern oleh main.cpp dan
//                 taskMqttPublish untuk monitoring
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "MeshPackets.h"
#include "MeshRouting.h"
#include "Network_Mqtt.h"
#include "Watchdog.h"

// g_mqttQueue dimiliki oleh file ini
QueueHandle_t g_mqttQueue = nullptr;

// g_rawQueue dimiliki oleh EspNowMesh.cpp
extern QueueHandle_t g_rawQueue;

// g_mqtt didefinisikan di main.cpp
extern NetworkMqtt g_mqtt;

static constexpr char TAG_HANDLER[] = "HANDLER";
static constexpr char TAG_PUBLISH[] = "PUBLISH";


// =============================================================================
// taskMeshHandler — Core 1
// =============================================================================
void taskMeshHandler(void* param)
{
    g_watchdog.registerTask();

    RawPacket   raw{};
    MqttMessage msg{};

    uint32_t receivedCount    = 0;
    uint32_t droppedCount     = 0;    // benar-benar drop (error)
    uint32_t accumulatingCount = 0;   // akumulasi normal
    uint32_t publishedCount   = 0;    // berhasil ke mqttQueue
    uint32_t lastLogMs        = 0;

    // interval counter
    uint32_t iRecv = 0, iDrop = 0, iAccum = 0, iPub = 0;

    LOG_INFO(TAG_HANDLER, "taskMeshHandler dimulai (Core 1)");

    for (;;)
    {
        g_watchdog.feed();

        if (xQueueReceive(g_rawQueue, &raw, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        receivedCount++;
        iRecv++;

        RouteResult result = MeshRouting::route(raw, msg);

        if (result == RouteResult::ACCUMULATING)
        {
            accumulatingCount++;
            iAccum++;
            continue; // normal, tidak perlu log warning
        }

        if (result == RouteResult::DROPPED)
        {
            droppedCount++;
            iDrop++;
            LOG_EVERY_N(20, LOG_WARN, TAG_HANDLER,
                        "Paket invalid dibuang (total=%lu)", droppedCount);
            continue;
        }

        // PUBLISHED — kirim ke mqttQueue
        publishedCount++;
        iPub++;

        if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
        {
            droppedCount++;
            iDrop++;
            LOG_EVERY_N(20, LOG_WARN, TAG_HANDLER,
                        "mqttQueue penuh — paket dibuang (total=%lu)",
                        droppedCount);
        }

        // Log interval setiap 10 detik
        if (millis() - lastLogMs >= 10000)
        {
            lastLogMs = millis();

            // Hitung efisiensi akumulasi
            // Dari 7 paket per window, 6 adalah akumulasi dan 1 publish cs_imu
            // Jadi rasio normal: accum/(accum+pub) ≈ 6/7 = 85.7%
            float dropRate = iRecv > 0
                             ? 100.0f * iDrop / iRecv
                             : 0.0f;
            float accumRate = iRecv > 0
                              ? 100.0f * iAccum / iRecv
                              : 0.0f;

            LOG_INFO(TAG_HANDLER,
                     "10s | recv=%lu accum=%lu(%.0f%%) pub=%lu drop=%lu(%.1f%%) "
                     "| rawQ=%u mqttQ=%u",
                     iRecv, iAccum, accumRate,
                     iPub, iDrop, dropRate,
                     uxQueueMessagesWaiting(g_rawQueue),
                     uxQueueMessagesWaiting(g_mqttQueue));

            iRecv = iAccum = iPub = iDrop = 0;
        }

        LOG_EVERY_N(500, LOG_DEBUG, TAG_HANDLER,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));
    }
}


// =============================================================================
// taskMqttPublish — Core 0
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

        // Status log setiap 10 detik
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

        // Handle disconnect
        if (!g_mqtt.isConnected())
        {
            g_mqtt.tryReconnect();
            g_mqtt.loop();

            // Buang hanya jika queue benar-benar kritis (< 5 slot tersisa)
            if (uxQueueSpacesAvailable(g_mqttQueue) < 5)
            {
                xQueueReceive(g_mqttQueue, &msg, 0);
                LOG_EVERY_N(10, LOG_WARN, TAG_PUBLISH,
                            "Queue kritis saat offline — buang 1 (sisa=%u slot)",
                            uxQueueSpacesAvailable(g_mqttQueue));
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // DRAIN LOOP — publish sampai queue kosong atau max 10 per iterasi
        // Ini yang menyelesaikan masalah rate mismatch
        uint8_t published = 0;
        while (published < 10 &&
               xQueueReceive(g_mqttQueue, &msg, 0) == pdTRUE)
        {
            if (g_mqtt.publish(msg.topic, msg.payload))
            {
                published++;
            }
            else
            {
                // Publish gagal — kemungkinan koneksi putus di tengah drain
                // Kembalikan pesan ke depan queue tidak bisa di FreeRTOS,
                // jadi log saja dan lanjut — akan di-reconnect di iterasi berikut
                LOG_WARN(TAG_PUBLISH, "Publish gagal di tengah drain — skip");
                break;
            }
        }

        // Kalau tidak ada yang di-publish, tunggu sampai ada pesan baru
        if (published == 0)
        {
            xQueueReceive(g_mqttQueue, &msg,
                          pdMS_TO_TICKS(100)); // timeout pendek = lebih responsif
            if (g_mqtt.publish(msg.topic, msg.payload))
                published++;
        }

        // loop() wajib dipanggil agar PubSubClient jaga keepalive
        g_mqtt.loop();

        LOG_EVERY_N(200, LOG_DEBUG, TAG_PUBLISH,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));
    }
}