// =============================================================================
// Network_EspNow.cpp
//
// SENSOR (senderMode=true):
//   WiFi.mode(STA) → set channel=1 → esp_now_init → add peer gateway
//   Kirim CombinedPacket setiap SEND_INTERVAL_MS (throttled di main.cpp)
//
// GATEWAY (senderMode=false):
//   WiFi sudah konek (dilakukan NetworkMqtt::begin() sebelum fungsi ini).
//   [Item #5 ISR Offload]
//   onDataRecv ISR → hanya memcpy raw bytes → push ke g_rawQueue
//   taskSerialize (di main.cpp) → ambil dari g_rawQueue → format JSON
//                               → push ke g_mqttQueue
//   taskMqttPublish → publish ke broker (tidak berubah)
//
// Kenapa ISR harus minimal?
//   ESP-NOW callback berjalan di WiFi task context (setara ISR priority).
//   snprintf loop 32× + Serial.printf di ISR bisa makan 1–5ms →
//   watchdog idle task timeout → gateway restart loop.
//   memcpy 250 bytes ~1µs → aman.
// =============================================================================

#include "Network_EspNow.h"
#include "DataModels_CS.h"
#include <cstring>
#include <esp_wifi.h>

QueueHandle_t g_rawQueue  = nullptr;
QueueHandle_t g_mqttQueue = nullptr;

static NetworkEspNow* _instance = nullptr;

static constexpr uint8_t ESPNOW_CHANNEL = 1;

// ---------------------------------------------------------------------------
// Catatan batching:
// BatchBuffer dan logika batching sudah DIPINDAH ke taskSerialize di main.cpp.
// Di sini tidak ada state batching — onDataRecv hanya memcpy ke g_rawQueue.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
bool NetworkEspNow::begin(bool senderMode) {
    _instance   = this;
    _senderMode = senderMode;

    if (senderMode) {
        // ===== SENSOR NODE =====
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Channel sensor: %d %s\n",
                      ch, ch == ESPNOW_CHANNEL ? "OK" : "MISMATCH!");
    } else {
        // ===== GATEWAY NODE =====
        uint8_t ch; wifi_second_chan_t sch;
        esp_wifi_get_channel(&ch, &sch);
        Serial.printf("[ESP-NOW] Gateway channel (ikut router): %d\n", ch);

        if (ch != ESPNOW_CHANNEL) {
            Serial.printf("[ESP-NOW] WARN: channel gateway=%d, sensor dikunci ke %d!\n",
                          ch, ESPNOW_CHANNEL);
            Serial.printf("[ESP-NOW] Ubah ESPNOW_CHANNEL=%d lalu compile ulang sensor.\n", ch);
        }
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] ERROR: init gagal!");
        return false;
    }

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    if (senderMode) {
        if (!addPeer(MacAddr::GATEWAY)) return false;
        Serial.println("[ESP-NOW] Mode: SENDER → Gateway terdaftar");
    } else {
        if (!addPeer(MacAddr::NODE_A)) return false;
        if (!addPeer(MacAddr::NODE_B)) return false;
        Serial.println("[ESP-NOW] Mode: RECEIVER → Node A & B terdaftar");
        Serial.printf("[ESP-NOW] Batching: %s (size=%d)\n",
                      BatchConfig::BATCHING_ENABLED ? "AKTIF" : "NONAKTIF",
                      BatchConfig::BATCH_SIZE);
        Serial.println("[ESP-NOW] ISR mode: MINIMAL → serialisasi di taskSerialize");
    }

    Serial.printf("[ESP-NOW] MAC lokal: %s\n", WiFi.macAddress().c_str());
    return true;
}

// ---------------------------------------------------------------------------
bool NetworkEspNow::addPeer(const uint8_t* mac) {
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.printf("[ESP-NOW] ERROR: gagal tambah peer %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool NetworkEspNow::sendCombined(const CombinedPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(CombinedPacket));
    if (r != ESP_OK) {
        Serial.printf("[ESP-NOW] sendCombined GAGAL: esp_err=0x%X (%s)\n", r,
                      r == ESP_ERR_ESPNOW_NOT_INIT  ? "NOT_INIT"      :
                      r == ESP_ERR_ESPNOW_ARG        ? "ARG_INVALID"   :
                      r == ESP_ERR_ESPNOW_INTERNAL   ? "INTERNAL"      :
                      r == ESP_ERR_ESPNOW_NO_MEM     ? "NO_MEM"        :
                      r == ESP_ERR_ESPNOW_NOT_FOUND  ? "PEER_NOT_FOUND":
                      r == ESP_ERR_ESPNOW_IF         ? "INTERFACE_ERR" : "UNKNOWN");
    }
    return r == ESP_OK;
}

bool NetworkEspNow::sendHeartbeat(const HeartbeatPacket& pkt) {
    esp_err_t r = esp_now_send(MacAddr::GATEWAY,
                               reinterpret_cast<const uint8_t*>(&pkt),
                               sizeof(HeartbeatPacket));
    return r == ESP_OK;
}

// ---------------------------------------------------------------------------
void NetworkEspNow::onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (_instance) _instance->_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESP-NOW] WARN: pengiriman gagal (NACK)");
    }
}

// ---------------------------------------------------------------------------
// [Item #5 ISR Offload] onDataRecv — MINIMAL, hanya memcpy
//
// TIDAK boleh ada di sini:
//   ✗ snprintf / sprintf
//   ✗ Serial.printf / Serial.println
//   ✗ malloc / new
//   ✗ logika bisnis apapun
//
// Yang boleh:
//   ✓ memcpy (< 250 bytes, ~1µs)
//   ✓ xQueueSendFromISR
//   ✓ perbandingan integer sederhana
//
// Semua serialisasi JSON dan routing dipindah ke taskSerialize di main.cpp.
// ---------------------------------------------------------------------------
void NetworkEspNow::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1 || !g_rawQueue) return;

    RawPacket raw{};
    raw.len = static_cast<uint8_t>(len <= 250 ? len : 250);
    memcpy(raw.data, data, raw.len);
    memcpy(raw.src_mac, mac, 6);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t sent = xQueueSendFromISR(g_rawQueue, &raw, &xHigherPriorityTaskWoken);

    // Jika queue penuh, paket dibuang — lebih baik drop 1 paket daripada
    // block ISR. taskSerialize akan log drop rate via counter.
    (void)sent;

    // Yield ke task prioritas lebih tinggi jika ada yang terbangun
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
