#pragma once
// =============================================================================
// DataModels.h — Definisi Struct Payload ESP-NOW & Data Internal
// Semua struct HARUS packed agar ukuran di pengirim == penerima.
// ESP-NOW max payload: 250 bytes.
//
// Layout ukuran (verify sebelum deploy):
//   PacketHeader    =  6 bytes
//   ImuSample       = 28 bytes  (7 × float)
//   PpgSample       = 14 bytes  (4+4+4+1+1)
//   EdgeResult      =  2 bytes
//   CombinedPacket  = 50 bytes  ✓ << 250 bytes
//
// [Item #5 ISR Offload] Tambahan:
//   RawPacket       = 257 bytes — wrapper raw ESP-NOW bytes untuk g_rawQueue
//   RAM g_rawQueue  = 257 × 10 = ~2.5 KB  ← jauh lebih hemat dari MqttMessage
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Tipe paket — byte pertama setiap payload, dipakai untuk routing di gateway
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    IMU_DATA      = 0x01,
    PPG_DATA      = 0x02,
    COMBINED_DATA = 0x03,
    CS_AX         = 0x10,
    CS_AY         = 0x11,
    CS_AZ         = 0x12,
    CS_GX         = 0x13,
    CS_GY         = 0x14,
    CS_GZ         = 0x15,
    CS_IR         = 0x16,
    HEARTBEAT     = 0xFF,
};

// ---------------------------------------------------------------------------
// Header umum — ada di setiap paket
// ---------------------------------------------------------------------------
struct __attribute__((packed)) PacketHeader {
    PacketType type;       // 1 byte — jenis paket
    uint8_t    node_id;    // 1 byte — ID node pengirim (1, 2, 3, ...)
    uint32_t   timestamp;  // 4 byte — millis() saat data diambil
};                         // Total: 6 bytes

// ---------------------------------------------------------------------------
// Data IMU dari MPU6050
// ---------------------------------------------------------------------------
struct __attribute__((packed)) ImuSample {
    float accel_x;   // 4 byte — m/s²
    float accel_y;   // 4 byte
    float accel_z;   // 4 byte
    float gyro_x;    // 4 byte — °/s
    float gyro_y;    // 4 byte
    float gyro_z;    // 4 byte
    float temp_c;    // 4 byte — suhu dari sensor MPU
};                   // Total: 28 bytes

struct __attribute__((packed)) ImuPacket {
    PacketHeader header;
    ImuSample    data;
    // Total: 6 + 28 = 34 bytes
};

// ---------------------------------------------------------------------------
// Data PPG dari MAX30102
// ---------------------------------------------------------------------------
struct __attribute__((packed)) PpgSample {
    uint32_t red_raw;    // 4 byte — nilai LED merah
    uint32_t ir_raw;     // 4 byte — nilai LED inframerah (raw, tidak diproses)
    float    spo2;       // 4 byte — SpO2 hasil kalkulasi library (0–100%)
    int8_t   heart_rate; // 1 byte — BPM hasil kalkulasi library (-1 jika invalid)
    bool     valid;      // 1 byte — apakah pembacaan HR/SpO2 valid
};                       // Total: 14 bytes

struct __attribute__((packed)) PpgPacket {
    PacketHeader header;
    PpgSample    data;
    // Total: 6 + 14 = 20 bytes
};

// ---------------------------------------------------------------------------
// Edge Result — hasil mini edge computing di sensor node
// ---------------------------------------------------------------------------
struct __attribute__((packed)) EdgeResult {
    bool     finger_on;   // 1 byte — true jika IR > threshold (jari menempel)
    uint8_t  reserved;    // 1 byte — padding untuk alignment, siap expand
};                        // Total: 2 bytes

// ---------------------------------------------------------------------------
// CombinedPacket — 1 ESP-NOW frame berisi semua data dari 1 sensor node
// ---------------------------------------------------------------------------
struct __attribute__((packed)) CombinedPacket {
    PacketHeader header;  //  6 bytes
    ImuSample    imu;     // 28 bytes
    PpgSample    ppg;     // 14 bytes
    EdgeResult   edge;    //  2 bytes
};                        // Total: 50 bytes ✓

// ---------------------------------------------------------------------------
// Heartbeat — dikirim periodik untuk deteksi node mati
// ---------------------------------------------------------------------------
struct __attribute__((packed)) HeartbeatPacket {
    PacketHeader header;
    uint32_t     uptime_s;
    uint8_t      rssi;
};

// ---------------------------------------------------------------------------
// [Item #5 ISR Offload] RawPacket
//
// Dipakai oleh onDataRecv ISR untuk meneruskan data mentah ke taskSerialize
// tanpa melakukan serialisasi JSON di dalam ISR.
//
// Kenapa 250 bytes? — Itu batas maksimum payload ESP-NOW.
// Kenapa bukan pointer? — ISR tidak boleh alokasi heap (malloc/new),
//                         buffer fixed-size di stack queue lebih aman.
//
// Alur:
//   ISR → memcpy ke RawPacket → xQueueSendFromISR(g_rawQueue)
//   taskSerialize → xQueueReceive(g_rawQueue) → format JSON
//                 → xQueueSend(g_mqttQueue)
//
// RAM: sizeof(RawPacket) = 250 + 1 + 6 = 257 bytes
//      g_rawQueue(10)    = 257 × 10    = ~2.5 KB
// ---------------------------------------------------------------------------
struct RawPacket {
    uint8_t data[250];   // raw ESP-NOW payload (max 250 bytes)
    uint8_t len;         // panjang aktual data yang valid
    uint8_t src_mac[6];  // MAC address pengirim
};                       // Total: 257 bytes

// ---------------------------------------------------------------------------
// Wrapper union — memudahkan cast dari raw bytes ESP-NOW
// ---------------------------------------------------------------------------
union EspNowPayload {
    uint8_t         raw[250];
    ImuPacket       imu;
    PpgPacket       ppg;
    CombinedPacket  combined;
    HeartbeatPacket heartbeat;

    PacketType type() const { return static_cast<PacketType>(raw[0]); }
};

// ---------------------------------------------------------------------------
// MqttMessage — pesan internal antar FreeRTOS task di gateway
//
// ⚠ PERHATIAN UKURAN RAM:
//   sizeof(MqttMessage) × QueueLen::MQTT_MSG = total heap queue
//   Payload 420 bytes = cukup untuk cs_ir (~360B) + margin 60B
//   RAM queue: 30 × (80 + 420) = 15,000 bytes = 15 KB ← AMAN
//   JANGAN naikkan tanpa hitung ulang!
// ---------------------------------------------------------------------------
struct MqttMessage {
    char topic[80];
    char payload[420];
};