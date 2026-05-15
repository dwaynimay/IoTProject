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

    uint32_t receivedCount = 0;
    uint32_t droppedCount  = 0;
    uint32_t lastLogMs     = 0;

    LOG_INFO(TAG_HANDLER, "taskMeshHandler dimulai (Core 1)");

    for (;;)
    {
        g_watchdog.feed();

        if (xQueueReceive(g_rawQueue, &raw, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        receivedCount++;

        if (!MeshRouting::route(raw, msg))
        {
            droppedCount++;
            continue;
        }

        if (xQueueSend(g_mqttQueue, &msg, 0) != pdTRUE)
        {
            droppedCount++;
            LOG_EVERY_N(20, LOG_WARN, TAG_HANDLER,
                        "g_mqttQueue penuh — paket dibuang (total drop=%lu)",
                        droppedCount);
        }

        if (millis() - lastLogMs >= 10000)
        {
            lastLogMs = millis();
            LOG_INFO(TAG_HANDLER,
                     "Throughput | recv=%lu drop=%lu | rawQ=%u mqttQ=%u",
                     receivedCount, droppedCount,
                     uxQueueMessagesWaiting(g_rawQueue),
                     uxQueueMessagesWaiting(g_mqttQueue));
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

        if (!g_mqtt.isConnected())
        {
            g_mqtt.tryReconnect();

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

        if (xQueueReceive(g_mqttQueue, &msg,
                          pdMS_TO_TICKS(Timing::MQTT_PUBLISH_MS)) == pdTRUE)
        {
            g_mqtt.publish(msg.topic, msg.payload);
        }

        g_mqtt.loop();

        LOG_EVERY_N(100, LOG_DEBUG, TAG_PUBLISH,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));
    }
}