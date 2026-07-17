#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include "Config.h"
#include "EspNowMesh.h"
#include "MeshRouting.h"
#include "Network_Mqtt.h"
#include "../Routing/DynamicRouter.h"

#ifndef TEST_PHASE_MS
    #define TEST_PHASE_MS 20000UL
#endif

#ifndef TEST_SEND_MS
    #define TEST_SEND_MS 3000UL
#endif

#ifndef TEST_BEACON_MS
    #define TEST_BEACON_MS 2000UL
#endif

#ifndef TEST_SCENARIO_ID
    #define TEST_SCENARIO_ID 2
#endif

#ifndef TEST_REPEAT_COUNT
    #define TEST_REPEAT_COUNT 5U
#endif

#ifndef TEST_RANDOM_JITTER_DBM
    #define TEST_RANDOM_JITTER_DBM 3
#endif

QueueHandle_t g_mqttQueue = nullptr;
DynamicRouter* g_routerPtr = nullptr;
EspNowMesh g_mesh;

#if NODE_ROLE == ROLE_GATEWAY
NetworkMqtt g_mqtt;
#endif

namespace TestCfg
{
    struct PhaseProfile
    {
        const char* name;
        int8_t senderSelfBaseRssi;
        int8_t relayBaseRssi;
        bool expectDirect;
    };

    static constexpr PhaseProfile kPhases[] = {
        {"baseline_direct", -48, -56, true},
        {"forced_relay",    -82, -43, false},
        {"relay_hold",      -86, -41, false},
        {"direct_recovery", -50, -62, true},
    };

    static constexpr uint8_t kPhaseCount =
        sizeof(kPhases) / sizeof(kPhases[0]);
}

struct TestContext
{
    uint32_t lastPhasePrint = 0;
    uint32_t imuSeq = 0;
    uint32_t ppgSeq = 0;
    uint8_t lastPhaseIndex = 255;
    uint32_t cycleCount = 0;
    int8_t activeSelfRssi = RoutingCfg::RSSI_UNKNOWN;
    int8_t activeRelayRssi = RoutingCfg::RSSI_UNKNOWN;
} g_test;

static uint8_t currentPhaseIndex()
{
    const uint32_t phaseWindow = millis() / TEST_PHASE_MS;
    return static_cast<uint8_t>(phaseWindow % TestCfg::kPhaseCount);
}

static uint32_t currentCycleNumber()
{
    const uint32_t phaseWindow = millis() / TEST_PHASE_MS;
    return (phaseWindow / TestCfg::kPhaseCount) + 1U;
}

static const TestCfg::PhaseProfile& currentPhase()
{
    return TestCfg::kPhases[currentPhaseIndex()];
}

static const char* routeLabel(bool isDirect)
{
    return isDirect ? "DIRECT" : "RELAY";
}

static const uint8_t* nextHopMac(uint8_t nextHopNodeId)
{
    if (nextHopNodeId == 2) return MacAddr::NODE_PPG;
    if (nextHopNodeId == 1) return MacAddr::NODE_IMU;
    return MacAddr::GATEWAY;
}

static int8_t jitteredRssi(int8_t baseValue)
{
    const int32_t jitter = random(-TEST_RANDOM_JITTER_DBM,
                                  TEST_RANDOM_JITTER_DBM + 1);
    return static_cast<int8_t>(static_cast<int32_t>(baseValue) + jitter);
}

static void refreshPhaseRssiIfNeeded()
{
    const uint8_t idx = currentPhaseIndex();
    if (g_test.lastPhaseIndex == idx &&
        g_test.activeSelfRssi != RoutingCfg::RSSI_UNKNOWN &&
        g_test.activeRelayRssi != RoutingCfg::RSSI_UNKNOWN)
    {
        return;
    }

    g_test.lastPhaseIndex = idx;
    g_test.cycleCount = currentCycleNumber();
    const auto& phase = currentPhase();
    g_test.activeSelfRssi = jitteredRssi(phase.senderSelfBaseRssi);
    g_test.activeRelayRssi = jitteredRssi(phase.relayBaseRssi);
}

static bool testFinished()
{
    return currentCycleNumber() > TEST_REPEAT_COUNT;
}

static void printPhaseBannerIfChanged()
{
    refreshPhaseRssiIfNeeded();
    const uint8_t idx = currentPhaseIndex();
    if (g_test.lastPhasePrint == idx && millis() > 2000) return;

    g_test.lastPhasePrint = idx;
    const auto& phase = currentPhase();
    Serial.printf(
        "[PHASE] node=%u scenario=%u cycle=%lu/%u idx=%u name=%s self=%d neighbor=%d expect=%s t=%lu\n",
        NODE_ID, TEST_SCENARIO_ID,
        (unsigned long)g_test.cycleCount, TEST_REPEAT_COUNT,
        idx, phase.name,
        g_test.activeSelfRssi, g_test.activeRelayRssi,
        routeLabel(phase.expectDirect),
        (unsigned long)millis());
}

static void fillDummyAxis(float out[CS_M], float base, uint32_t seq, uint8_t axisId)
{
    for (uint8_t i = 0; i < CS_M; ++i)
    {
        const float x = static_cast<float>(i) * 0.18f;
        out[i] = base + 0.2f * sinf(x + axisId) + 0.03f * static_cast<float>(seq);
    }
}

static void fillDummyPpg(float out[CS_M], uint32_t seq)
{
    for (uint8_t i = 0; i < CS_M; ++i)
    {
        const float x = static_cast<float>(i) * 0.22f;
        out[i] = 0.55f + 0.25f * sinf(x) + 0.08f * cosf(x * 0.5f) +
                 0.01f * static_cast<float>(seq % 10U);
    }
}

#if NODE_ROLE != ROLE_GATEWAY
static void taskRssiReporter(void* param)
{
    while (!g_mesh.isChannelConfirmed()) vTaskDelay(pdMS_TO_TICKS(500));

    for (;;)
    {
        refreshPhaseRssiIfNeeded();
        if (testFinished())
        {
            Serial.printf("[DONE] node=%u cycle=%lu imu_seq=%lu ppg_seq=%lu t=%lu\n",
                          NODE_ID,
                          (unsigned long)(g_test.cycleCount - 1U),
                          (unsigned long)g_test.imuSeq,
                          (unsigned long)g_test.ppgSeq,
                          (unsigned long)millis());
            vTaskDelete(nullptr);
        }

        int8_t reportRssi = (NODE_ID == 2)
            ? g_test.activeRelayRssi
            : g_test.activeSelfRssi;

        if (NODE_ID == 1 && g_routerPtr)
        {
            g_routerPtr->updateSelfRssi(g_test.activeSelfRssi);
            g_routerPtr->updateNeighborRssi(2, g_test.activeRelayRssi);
        }

        const bool ok = g_mesh.sendRssiReport(NODE_ID, reportRssi);
        Serial.printf("[PLAN] node=%u cycle=%lu phase=%s report_rssi=%d ok=%d t=%lu\n",
                      NODE_ID, (unsigned long)g_test.cycleCount, currentPhase().name,
                      reportRssi, ok ? 1 : 0, (unsigned long)millis());
        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_MS));
    }
}
#endif

static void setupGateway()
{
#if NODE_ROLE == ROLE_GATEWAY
    Serial.println("\n=======================================");
    Serial.println("  AUTO MESH END-TO-END TEST: GATEWAY");
    Serial.println("=======================================");

    if (!g_mqtt.begin())
    {
        Serial.println("[ERR] MQTT begin gagal");
    }

    if (!g_mesh.begin(false))
    {
        Serial.println("[ERR] ESP-NOW begin gagal");
    }

    uint8_t ch = 0;
    wifi_second_chan_t sch;
    if (esp_wifi_get_channel(&ch, &sch) != ESP_OK || ch == 0)
        ch = static_cast<uint8_t>(WiFi.channel());
    if (ch > 0 && ch <= 13)
        g_mesh.setGatewayChannel(ch);
#endif
}

static void setupSensor()
{
#if NODE_ROLE != ROLE_GATEWAY
    Serial.println("\n=======================================");
    Serial.printf("  AUTO MESH END-TO-END TEST: SENSOR NODE %u\n", NODE_ID);
    Serial.println("=======================================");

    g_routerPtr = new DynamicRouter(NODE_ID);
    g_mesh.begin(true);
    xTaskCreatePinnedToCore(taskRssiReporter, "RSSI_SCRIPT", 4096, nullptr, 1,
                            nullptr, 0);
#endif
}

static void sendImuWindow()
{
#if NODE_ROLE == ROLE_SENSOR_IMU
    refreshPhaseRssiIfNeeded();
    if (testFinished() || !g_mesh.isChannelConfirmed() || !g_routerPtr) return;

    g_routerPtr->updateSelfRssi(g_test.activeSelfRssi);
    g_routerPtr->updateNeighborRssi(2, g_test.activeRelayRssi);
    const RouteDecision dec = g_routerPtr->decide();
    const uint8_t* dstMac = dec.isDirect ? MacAddr::GATEWAY
                                         : nextHopMac(dec.nextHopNodeId);
    const uint32_t seq = ++g_test.imuSeq;
    const uint32_t tsNow = millis();

    float ax[CS_M], ay[CS_M], az[CS_M], gx[CS_M], gy[CS_M], gz[CS_M];
    fillDummyAxis(ax, 0.10f, seq, 0);
    fillDummyAxis(ay, 0.20f, seq, 1);
    fillDummyAxis(az, 0.30f, seq, 2);
    fillDummyAxis(gx, 0.40f, seq, 3);
    fillDummyAxis(gy, 0.50f, seq, 4);
    fillDummyAxis(gz, 0.60f, seq, 5);

    bool okAll = true;
    okAll &= g_mesh.sendCsAxis(PKT_CS_AX, NODE_ID, ax, 0.10f, true, tsNow, dstMac);
    okAll &= g_mesh.sendCsAxis(PKT_CS_AY, NODE_ID, ay, 0.20f, true, tsNow, dstMac);
    okAll &= g_mesh.sendCsAxis(PKT_CS_AZ, NODE_ID, az, 0.30f, true, tsNow, dstMac);
    okAll &= g_mesh.sendCsAxis(PKT_CS_GX, NODE_ID, gx, 0.40f, true, tsNow, dstMac);
    okAll &= g_mesh.sendCsAxis(PKT_CS_GY, NODE_ID, gy, 0.50f, true, tsNow, dstMac);
    okAll &= g_mesh.sendCsAxis(PKT_CS_GZ, NODE_ID, gz, 0.60f, true, tsNow, dstMac);

    Serial.printf(
        "[TX_IMU] node=%u scenario=%u cycle=%lu/%u phase=%s seq=%lu expect=%s actual=%s next_hop=%u ok=%d ts=%lu t=%lu\n",
        NODE_ID, TEST_SCENARIO_ID, (unsigned long)g_test.cycleCount, TEST_REPEAT_COUNT,
        currentPhase().name, (unsigned long)seq,
        routeLabel(currentPhase().expectDirect), routeLabel(dec.isDirect),
        dec.nextHopNodeId, okAll ? 1 : 0, (unsigned long)tsNow, (unsigned long)millis());
#endif
}

static void sendPpgWindow()
{
#if NODE_ROLE == ROLE_SENSOR_PPG
    refreshPhaseRssiIfNeeded();
    if (testFinished() || !g_mesh.isChannelConfirmed()) return;

    const uint32_t seq = ++g_test.ppgSeq;
    const uint32_t tsNow = millis();
    float yIr[CS_M];
    fillDummyPpg(yIr, seq);
    const bool ok = g_mesh.sendCsPpg(NODE_ID, yIr, 0.55f, 79, true, 98.0f, true,
                                     tsNow, MacAddr::GATEWAY);

    Serial.printf(
        "[TX_PPG] node=%u scenario=%u cycle=%lu/%u phase=%s seq=%lu ok=%d ts=%lu t=%lu\n",
        NODE_ID, TEST_SCENARIO_ID, (unsigned long)g_test.cycleCount, TEST_REPEAT_COUNT,
        currentPhase().name, (unsigned long)seq, ok ? 1 : 0,
        (unsigned long)tsNow, (unsigned long)millis());
#endif
}

static void processIncomingPackets()
{
    RawPacket raw;
    if (!g_mesh.readPacket(raw)) return;

#if NODE_ROLE == ROLE_GATEWAY
    MqttMessage mqttMsg;
    const RouteResult res = MeshRouting::route(raw, mqttMsg, nullptr);
    if (res == RouteResult::PUBLISHED)
    {
        const bool relayed = strstr(mqttMsg.payload, "\"relayed_by\"") != nullptr;
        const bool mqttOk = g_mqtt.publish(mqttMsg.topic, mqttMsg.payload);
        Serial.printf("[MQTT] route=%s topic=%s ok=%d payload_len=%u t=%lu\n",
                      relayed ? "RELAYED" : "DIRECT",
                      mqttMsg.topic, mqttOk ? 1 : 0,
                      static_cast<unsigned>(strlen(mqttMsg.payload)),
                      (unsigned long)millis());
    }
#else
    if (g_routerPtr)
    {
        MqttMessage ignored;
        MeshRouting::route(raw, ignored, g_routerPtr);
    }

    if (NODE_ID == 2 && raw.data[0] >= PKT_CS_AX && raw.data[0] <= PKT_CS_GZ)
    {
        const auto* hdr = reinterpret_cast<const PacketHeader*>(raw.data);
        const bool ok = g_mesh.forwardRoutedCs(NODE_ID, hdr->nodeId, raw.data, raw.len);
        Serial.printf("[RELAY] node=%u original=%u ok=%d type=%u t=%lu\n",
                      NODE_ID, hdr->nodeId, ok ? 1 : 0, raw.data[0],
                      (unsigned long)millis());
    }
#endif
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    randomSeed(static_cast<uint32_t>(esp_random()) ^ millis() ^
               static_cast<uint32_t>(NODE_ID * 991U));

#if NODE_ROLE == ROLE_GATEWAY
    setupGateway();
#else
    setupSensor();
#endif

    printPhaseBannerIfChanged();
}

void loop()
{
    printPhaseBannerIfChanged();

    if (testFinished())
    {
        Serial.printf("[DONE] node=%u cycle=%u imu_seq=%lu ppg_seq=%lu t=%lu\n",
                      NODE_ID, TEST_REPEAT_COUNT,
                      (unsigned long)g_test.imuSeq,
                      (unsigned long)g_test.ppgSeq,
                      (unsigned long)millis());
        delay(1000);
        return;
    }

#if NODE_ROLE == ROLE_GATEWAY
    static uint32_t lastBeacon = 0;
    if (millis() - lastBeacon >= TEST_BEACON_MS)
    {
        const bool ok = g_mesh.sendBeacon();
        Serial.printf("[PLAN] node=%u beacon_ok=%d t=%lu\n",
                      NODE_ID, ok ? 1 : 0, (unsigned long)millis());
        lastBeacon = millis();
    }
    g_mqtt.loop();
#elif NODE_ROLE == ROLE_SENSOR_IMU
    static uint32_t lastSendImu = 0;
    g_mesh.processPendingChannelSync();
    if (millis() - lastSendImu >= TEST_SEND_MS)
    {
        sendImuWindow();
        lastSendImu = millis();
    }
#elif NODE_ROLE == ROLE_SENSOR_PPG
    static uint32_t lastSendPpg = 0;
    g_mesh.processPendingChannelSync();
    if (millis() - lastSendPpg >= TEST_SEND_MS)
    {
        sendPpgWindow();
        lastSendPpg = millis();
    }
#endif

    processIncomingPackets();
}
