// File: firmware/test_sketches/test_mesh_routing.cpp
// Deskripsi: Test mandiri untuk ESP-NOW Mesh & Routing antar Node.
//
// CARA TES:
// 1. Hubungkan 2 buah ESP32 ke PC (atau sumber listrik).
// 2. Flash ESP32 pertama dengan: pio run -e test_mesh_gateway -t upload
// 3. Flash ESP32 kedua dengan:   pio run -e test_mesh_sensor -t upload
// 4. Buka Serial Monitor di kedua ESP32.
// 
// EKSPEKTASI:
// - Gateway akan broadcast Beacon setiap 2 detik.
// - Sensor akan menerima Beacon, menyamakan Channel, mencetak RSSI, lalu mulai mengirim Heartbeat.
// - Gateway akan menerima Heartbeat dan mencetak hasil routing-nya.

#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "EspNowMesh.h"
#include "MeshRouting.h"
#include "../Routing/DynamicRouter.h"

// =============================================================================
// TES RELAY — Suntik RSSI palsu agar decide() memilih RELAY.
//
// Definisikan FORCE_RELAY_TEST (lewat build_flags) HANYA pada node pengirim
// relay (mis. NODE_ID=1). Node lain dibiarkan DIRECT agar bisa jadi forwarder.
//
//   self     = -80 dBm  (seolah jauh dari gateway)
//   neighbor = -40 dBm  (seolah neighbor dekat gateway)
//   diff     = 40 dBm  >= RELAY_THRESHOLD_DBM (5)  → RELAY
// =============================================================================
#ifdef FORCE_RELAY_TEST
  #ifndef FORCE_RELAY_SELF_DBM
    #define FORCE_RELAY_SELF_DBM     (-80)
  #endif
  #ifndef FORCE_RELAY_NEIGHBOR_DBM
    #define FORCE_RELAY_NEIGHBOR_DBM (-40)
  #endif
#endif

// Deklarasi extern queue & pointer yang dibutuhkan EspNowMesh
QueueHandle_t g_mqttQueue;
DynamicRouter* g_routerPtr = nullptr;

EspNowMesh g_mesh;

#if NODE_ROLE != ROLE_GATEWAY
void taskRssiDummy(void* param) {
    while (!g_mesh.isChannelConfirmed()) vTaskDelay(1000);
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        int8_t myRssi = g_mesh.getLastBeaconRssi();
#ifndef FORCE_RELAY_TEST
        // Saat force relay, JANGAN timpa self-RSSI dengan nilai asli —
        // suntikan dilakukan di loop() agar konsisten tiap window.
        if (myRssi != RoutingCfg::RSSI_UNKNOWN) g_routerPtr->updateSelfRssi(myRssi);
#endif
        g_mesh.sendRssiReport(NODE_ID, myRssi);
    }
}
#endif

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    // Alokasi memori Queue
    g_mqttQueue = xQueueCreate(20, sizeof(MqttMessage));

#if NODE_ROLE == ROLE_GATEWAY
    Serial.println("\n\n=======================================");
    Serial.println("  TEST MESH: GATEWAY NODE");
    Serial.println("=======================================");
    
    // PENTING: Gateway di kode utama diasumsikan sudah menyalakan WiFi
    // lewat MQTT. Karena ini tes standalone, kita harus nyalakan WiFi manual.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Mode senderMode = false (Bisa terima dari semua)
    g_mesh.begin(false);
    
    // Set channel sementara ke 1 (karena WiFi belum nyala di tes ini)
    g_mesh.setGatewayChannel(1);
#else
    Serial.println("\n\n=======================================");
    Serial.printf("  TEST MESH: SENSOR NODE (ID: %d)\n", NODE_ID);
    Serial.println("=======================================");

    g_routerPtr = new DynamicRouter(NODE_ID);
    
    // Mode senderMode = true
    g_mesh.begin(true);

    xTaskCreatePinnedToCore(taskRssiDummy, "RSSI_DUMMY", 4096, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
#if NODE_ROLE == ROLE_GATEWAY
    // [GATEWAY] Rutin Broadcast Beacon untuk memandu sensor
    static uint32_t lastBeacon = 0;
    if (millis() - lastBeacon > 2000) {
        g_mesh.sendBeacon();
        Serial.println("[GATEWAY] Memancarkan Beacon...");
        lastBeacon = millis();
    }
#else
    // [SENSOR] Cek channel dan kirim data dummy (Heartbeat) jika konek
    static uint32_t lastSend = 0;
    if (millis() - lastSend > 3000) {
        if (g_mesh.isChannelConfirmed()) {
            int8_t rssi = g_mesh.getLastBeaconRssi();
            
#ifdef FORCE_RELAY_TEST
            // Suntik RSSI palsu tiap window agar decide() konsisten RELAY.
            g_routerPtr->updateSelfRssi(FORCE_RELAY_SELF_DBM);
            g_routerPtr->updateNeighborRssi(g_routerPtr->neighborNodeId(),
                                            FORCE_RELAY_NEIGHBOR_DBM);
#endif

            RouteDecision dec = g_routerPtr->decide();
            const uint8_t* dstMac = dec.isDirect ? MacAddr::GATEWAY : 
                                   ((NODE_ID == 1) ? MacAddr::NODE_PPG : MacAddr::NODE_IMU);

            Serial.printf("[SENSOR] Mengirim dummy CS Data via %s (RSSI: %d dBm)...\n", 
                          dec.isDirect ? "DIRECT" : "RELAY", rssi);
            
            // Kirim paket dummy CS (mirip seperti CS PPG)
            float dummyY[32] = {0};
            dummyY[0] = NODE_ID; // Tanda ini paket dari siapa
            g_mesh.sendCsPpg(NODE_ID, dummyY, 75, true, 99.0f, true, millis(), dstMac);
            
        } else {
            Serial.printf("[SENSOR] Menunggu Beacon dari Gateway untuk Sinkronisasi Channel...\n");
        }
        lastSend = millis();
    }
    
    // [SENSOR] Update channel secara aman (tidak di dalam Interrupt ISR)
    g_mesh.processPendingChannelSync();
#endif

    // [KEDUANYA] Cek jika ada paket masuk di antrean
    RawPacket raw;
    if (g_mesh.readPacket(raw)) {
        Serial.printf("\n--> [RECV] Paket masuk dari MAC [%02X:%02X:%02X:%02X:%02X:%02X] | Tipe: %d\n",
                      raw.srcMac[0], raw.srcMac[1], raw.srcMac[2], raw.srcMac[3], raw.srcMac[4], raw.srcMac[5],
                      raw.data[0]);

#if NODE_ROLE == ROLE_GATEWAY
        // Proses ke routing engine (di gateway)
        MqttMessage mqttMsg;
        RouteResult res = MeshRouting::route(raw, mqttMsg, nullptr);
        
        if (res == RouteResult::PUBLISHED) {
            Serial.printf("    [ROUTING] Paket Berhasil Diurai -> Siap dikirim ke MQTT Topic: %s\n", mqttMsg.topic);
            Serial.printf("    [ROUTING] Payload: %s\n", mqttMsg.payload);
        } else if (res == RouteResult::ACCUMULATING) {
            Serial.println("    [ROUTING] Paket tipe CS (diakumulasi sebelum dikirim ke Topic gabungan).");
        }
#else
        // SENSOR NODE: Jika menerima paket CS dari teman, FORWARD ke Gateway!
        if (raw.data[0] >= PKT_CS_AX && raw.data[0] <= PKT_CS_IR) {
            Serial.printf("    [RELAY] Mem-forward paket tipe %d dari teman ke Gateway...\n", raw.data[0]);
            g_mesh.forwardRoutedCs(NODE_ID, raw.data[1], raw.data, raw.len);
        }
#endif
    }
}
