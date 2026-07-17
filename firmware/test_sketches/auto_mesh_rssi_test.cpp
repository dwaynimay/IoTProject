#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "EspNowMesh.h"
#include "MeshRouting.h"
#include "../Routing/DynamicRouter.h"

// =============================================================================
// AUTO MESH RSSI TEST
// =============================================================================
//
// Why this sketch exists:
//   Menjalankan pembuktian routing DIRECT vs RELAY tanpa memindahkan node
//   secara fisik. Setiap node mengikuti script fase waktu yang menyuntik
//   RSSI palsu sehingga keputusan routing berubah otomatis selama pengujian.
//
// Test flow:
//   1. Gateway memancarkan beacon periodik.
//   2. Node pengirim (NODE_ID=1) menyuntik self RSSI + neighbor RSSI per fase.
//   3. Node relay (NODE_ID=2) mengirim RSSI report scripted agar node pengirim
//      menerima nilai neighbor yang konsisten.
//   4. Node pengirim mengirim dummy packet CS PPG dengan route yang diputuskan
//      DynamicRouter.
//   5. Gateway mencatat apakah paket datang direct atau melalui relay.
//
// Logging:
//   Semua node mencetak satu baris log ringkas per event dengan prefix:
//   [PLAN], [TX], [RX], [GW], [RELAY], [PHASE]
//
// Upload pairing:
//   - env:test_mesh_auto_gateway   -> gateway
//   - env:test_mesh_auto_sensor_n1 -> node pengirim
//   - env:test_mesh_auto_sensor_n2 -> node relay
// =============================================================================

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
    #define TEST_SCENARIO_ID 1
#endif

#ifndef TEST_REPEAT_COUNT
    #define TEST_REPEAT_COUNT 5U
#endif

#ifndef TEST_RANDOM_JITTER_DBM
    #define TEST_RANDOM_JITTER_DBM 3
#endif

QueueHandle_t g_mqttQueue;
DynamicRouter* g_routerPtr = nullptr;
EspNowMesh g_mesh;

namespace Metrics
{
    static constexpr uint16_t RAW_WINDOW_BYTES = CS_N * sizeof(float);
    static constexpr uint16_t CS_MEASUREMENT_BYTES = CS_M * sizeof(float);
}

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
    uint32_t txSeq = 0;
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

static const uint8_t* nextHopMac(uint8_t nextHopNodeId)
{
    if (nextHopNodeId == 2) return MacAddr::NODE_PPG;
    if (nextHopNodeId == 1) return MacAddr::NODE_IMU;
    return MacAddr::GATEWAY;
}

static const char* routeLabel(bool isDirect)
{
    return isDirect ? "DIRECT" : "RELAY";
}

static uint8_t phaseNumber()
{
    return static_cast<uint8_t>(currentPhaseIndex() + 1U);
}

static int8_t jitteredRssi(int8_t baseValue)
{
    const int32_t jitter = random(-TEST_RANDOM_JITTER_DBM,
                                  TEST_RANDOM_JITTER_DBM + 1);
    const int32_t value = static_cast<int32_t>(baseValue) + jitter;
    return static_cast<int8_t>(value);
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
        NODE_ID,
        TEST_SCENARIO_ID,
        (unsigned long)g_test.cycleCount,
        TEST_REPEAT_COUNT,
        idx,
        phase.name,
        g_test.activeSelfRssi,
        g_test.activeRelayRssi,
        routeLabel(phase.expectDirect),
        (unsigned long)millis());
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
            Serial.printf("[DONE] node=%u cycles=%lu tx=%lu t=%lu\n",
                          NODE_ID,
                          (unsigned long)(g_test.cycleCount - 1U),
                          (unsigned long)g_test.txSeq,
                          (unsigned long)millis());
            vTaskDelete(nullptr);
        }

        const auto& phase = currentPhase();
        int8_t reportRssi = g_mesh.getLastBeaconRssi();

        if (NODE_ID == 2)
        {
            // Relay node sengaja mengirim RSSI scripted agar node pengirim
            // menerima kualitas link neighbor->gateway yang konsisten.
            reportRssi = g_test.activeRelayRssi;
        }

        if (NODE_ID == 1)
        {
            // Karena ini simulasi, Node 1 langsung mengetahui kualitas link
            // Node 2 (neighbor) ke gateway tanpa harus menunggu packet RSSI_REPORT.
            // Ini memastikan pengujian logika routing terisolasi dari packet loss.
            g_routerPtr->updateSelfRssi(g_test.activeSelfRssi);
            g_routerPtr->updateNeighborRssi(2, g_test.activeRelayRssi);
        }

        const bool ok = g_mesh.sendRssiReport(NODE_ID, reportRssi);
        Serial.printf(
            "[PLAN] node=%u cycle=%lu phase=%s report_rssi=%d ok=%d t=%lu\n",
            NODE_ID,
            (unsigned long)g_test.cycleCount,
            phase.name,
            reportRssi,
            ok ? 1 : 0,
            (unsigned long)millis());

        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_MS));
    }
}
#endif

static void setupGateway()
{
    Serial.println("\n=======================================");
    Serial.println("  AUTO MESH RSSI TEST: GATEWAY");
    Serial.println("=======================================");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    g_mesh.begin(false);
    g_mesh.setGatewayChannel(6);
}

static void setupSensor()
{
    Serial.println("\n=======================================");
    Serial.printf("  AUTO MESH RSSI TEST: SENSOR NODE %u\n", NODE_ID);
    Serial.println("=======================================");

    g_routerPtr = new DynamicRouter(NODE_ID);
    g_mesh.begin(true);

#if NODE_ROLE != ROLE_GATEWAY
    xTaskCreatePinnedToCore(taskRssiReporter, "RSSI_SCRIPT", 4096, nullptr, 1,
                            nullptr, 0);
#endif
}

static void sendScriptedPacket()
{
#if NODE_ROLE == ROLE_GATEWAY
    return;
#else
    refreshPhaseRssiIfNeeded();
    if (testFinished()) return;
    if (NODE_ID != 1 || !g_mesh.isChannelConfirmed() || !g_routerPtr) return;

    const auto& phase = currentPhase();
    g_routerPtr->updateSelfRssi(g_test.activeSelfRssi);

    const RouteDecision dec = g_routerPtr->decide();
    const uint8_t* dstMac = dec.isDirect ? MacAddr::GATEWAY
                                         : nextHopMac(dec.nextHopNodeId);

    float dummyY[CS_M] = {0.0f};
    dummyY[0] = static_cast<float>(TEST_SCENARIO_ID);
    dummyY[1] = static_cast<float>(g_test.cycleCount);
    dummyY[2] = static_cast<float>(currentPhaseIndex());
    dummyY[3] = static_cast<float>(++g_test.txSeq);
    dummyY[4] = static_cast<float>(g_test.activeSelfRssi);
    dummyY[5] = static_cast<float>(g_test.activeRelayRssi);

    const bool ok = g_mesh.sendCsPpg(
        NODE_ID, dummyY, 75.0f, 88, true, 98.0f, true, millis(), dstMac);

    const size_t packetBytes = dec.isDirect
        ? sizeof(CSPpgPacket)
        : sizeof(RoutedCsHeader) + sizeof(CSPpgPacket);
    const int32_t savedBytes =
        static_cast<int32_t>(Metrics::RAW_WINDOW_BYTES) -
        static_cast<int32_t>(Metrics::CS_MEASUREMENT_BYTES);
    const float compressionPct =
        (static_cast<float>(savedBytes) / static_cast<float>(Metrics::RAW_WINDOW_BYTES)) * 100.0f;

    Serial.printf(
        "[TX] node=%u scenario=%u cycle=%lu/%u phase=%s phase_idx=%u seq=%lu expect=%s actual=%s self=%d neighbor=%d next_hop=%u ok=%d raw_bytes=%u cs_bytes=%u pkt_bytes=%u saved_bytes=%ld comp_pct=%.1f t=%lu\n",
        NODE_ID,
        TEST_SCENARIO_ID,
        (unsigned long)g_test.cycleCount,
        TEST_REPEAT_COUNT,
        phase.name,
        phaseNumber(),
        (unsigned long)g_test.txSeq,
        routeLabel(phase.expectDirect),
        routeLabel(dec.isDirect),
        dec.rssiSelf,
        dec.rssiNeighbor,
        dec.nextHopNodeId,
        ok ? 1 : 0,
        Metrics::RAW_WINDOW_BYTES,
        Metrics::CS_MEASUREMENT_BYTES,
        static_cast<unsigned>(packetBytes),
        static_cast<long>(savedBytes),
        compressionPct,
        (unsigned long)millis());
#endif
}

struct PacketMeta
{
    bool valid = false;
    uint8_t nodeId = 0;
    uint8_t scenario = 0;
    uint8_t cycle = 0;
    uint8_t phaseIdx = 0;
    uint32_t seq = 0;
    int8_t selfRssi = 0;
    int8_t neighborRssi = 0;
    uint32_t sourceTs = 0;
    size_t packetBytes = 0;
};

static PacketMeta extractPacketMeta(const RawPacket& raw)
{
    PacketMeta meta{};

    if (raw.data[0] == PKT_CS_IR && raw.len >= sizeof(CSPpgPacket))
    {
        const auto* pkt = reinterpret_cast<const CSPpgPacket*>(raw.data);
        meta.valid = true;
        meta.nodeId = pkt->header.nodeId;
        meta.scenario = static_cast<uint8_t>(pkt->yIr[0]);
        meta.cycle = static_cast<uint8_t>(pkt->yIr[1]);
        meta.phaseIdx = static_cast<uint8_t>(pkt->yIr[2]) + 1U;
        meta.seq = static_cast<uint32_t>(pkt->yIr[3]);
        meta.selfRssi = static_cast<int8_t>(pkt->yIr[4]);
        meta.neighborRssi = static_cast<int8_t>(pkt->yIr[5]);
        meta.sourceTs = pkt->header.timestamp;
        meta.packetBytes = raw.len;
        return meta;
    }

    if (raw.data[0] == static_cast<uint8_t>(PacketType::ROUTED_CS) &&
        raw.len >= (sizeof(RoutedCsHeader) + sizeof(CSPpgPacket)))
    {
        const auto* routedHdr = reinterpret_cast<const RoutedCsHeader*>(raw.data);
        const auto* inner = reinterpret_cast<const CSPpgPacket*>(raw.data + sizeof(RoutedCsHeader));
        if (inner->header.type == PacketType::CS_IR)
        {
            meta.valid = true;
            meta.nodeId = routedHdr->originalNodeId;
            meta.scenario = static_cast<uint8_t>(inner->yIr[0]);
            meta.cycle = static_cast<uint8_t>(inner->yIr[1]);
            meta.phaseIdx = static_cast<uint8_t>(inner->yIr[2]) + 1U;
            meta.seq = static_cast<uint32_t>(inner->yIr[3]);
            meta.selfRssi = static_cast<int8_t>(inner->yIr[4]);
            meta.neighborRssi = static_cast<int8_t>(inner->yIr[5]);
            meta.sourceTs = inner->header.timestamp;
            meta.packetBytes = raw.len;
        }
    }

    return meta;
}

static void processIncomingPackets()
{
    RawPacket raw;
    if (!g_mesh.readPacket(raw)) return;

    Serial.printf(
        "[RX] node=%u type=%u src=%02X:%02X:%02X:%02X:%02X:%02X len=%u t=%lu\n",
        NODE_ID,
        raw.data[0],
        raw.srcMac[0], raw.srcMac[1], raw.srcMac[2],
        raw.srcMac[3], raw.srcMac[4], raw.srcMac[5],
        raw.len,
        (unsigned long)millis());

#if NODE_ROLE == ROLE_GATEWAY
    const PacketMeta meta = extractPacketMeta(raw);
    MqttMessage mqttMsg;
    const RouteResult res = MeshRouting::route(raw, mqttMsg, nullptr);

    if (res == RouteResult::PUBLISHED)
    {
        const bool relayed = strstr(mqttMsg.payload, "\"relayed_by\"") != nullptr;
        const uint32_t gatewayNow = millis();
        const uint32_t latencyMs =
            meta.valid && gatewayNow >= meta.sourceTs ? (gatewayNow - meta.sourceTs) : 0U;
        Serial.printf(
            "[GW] route=%s node=%u scenario=%u cycle=%u phase_idx=%u seq=%lu latency_ms=%lu raw_bytes=%u cs_bytes=%u pkt_bytes=%u topic=%s payload=%s\n",
            relayed ? "RELAYED" : "DIRECT",
            meta.nodeId,
            meta.scenario,
            meta.cycle,
            meta.phaseIdx,
            (unsigned long)meta.seq,
            (unsigned long)latencyMs,
            Metrics::RAW_WINDOW_BYTES,
            Metrics::CS_MEASUREMENT_BYTES,
            static_cast<unsigned>(meta.packetBytes),
            mqttMsg.topic,
            mqttMsg.payload);
    }
    else if (res == RouteResult::ACCUMULATING)
    {
        Serial.println("[GW] packet_accumulating");
    }
    else
    {
        Serial.printf("[GW] route_result=%d\n", static_cast<int>(res));
    }
#else
    if (g_routerPtr)
    {
        MqttMessage ignored;
        MeshRouting::route(raw, ignored, g_routerPtr);
    }

    if (NODE_ID == 2 && raw.data[0] >= PKT_CS_AX && raw.data[0] <= PKT_CS_IR)
    {
        const bool ok = g_mesh.forwardRoutedCs(NODE_ID, raw.data[1], raw.data, raw.len);
        Serial.printf(
            "[RELAY] node=%u cycle=%lu original=%u phase=%s ok=%d t=%lu\n",
            NODE_ID,
            (unsigned long)g_test.cycleCount,
            raw.data[1],
            currentPhase().name,
            ok ? 1 : 0,
            (unsigned long)millis());
    }
#endif
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    randomSeed(static_cast<uint32_t>(esp_random()) ^ millis() ^
               static_cast<uint32_t>(NODE_ID * 997U));

    g_mqttQueue = xQueueCreate(20, sizeof(MqttMessage));
    g_test.lastPhasePrint = 255;

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
        Serial.printf("[DONE] node=%u cycles=%u tx=%lu t=%lu\n",
                      NODE_ID,
                      TEST_REPEAT_COUNT,
                      (unsigned long)g_test.txSeq,
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
#else
    static uint32_t lastSend = 0;
    g_mesh.processPendingChannelSync();

    if (millis() - lastSend >= TEST_SEND_MS)
    {
        sendScriptedPacket();
        lastSend = millis();
    }
#endif

    processIncomingPackets();
}
