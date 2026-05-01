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
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Tipe paket — byte pertama setiap payload, dipakai untuk routing di gateway
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    IMU_DATA      = 0x01,
    PPG_DATA      = 0x02,
    COMBINED_DATA = 0x03, // NEW: IMU + PPG + EdgeResult dalam satu frame
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
// Data asli (ImuSample, PpgSample) TIDAK diubah agar ML server bisa
// bekerja dengan raw signal. EdgeResult hanya metadata tambahan.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) EdgeResult {
    bool     finger_on;   // 1 byte — true jika IR > threshold (jari menempel)
    uint8_t  reserved;    // 1 byte — padding untuk alignment, siap expand
};                        // Total: 2 bytes

// ---------------------------------------------------------------------------
// CombinedPacket — 1 ESP-NOW frame berisi semua data dari 1 sensor node
// Gunakan ini sebagai pengganti ImuPacket + PpgPacket terpisah.
// Gateway routing berdasarkan header.node_id untuk pisahkan per ESP32.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) CombinedPacket {
    PacketHeader header;  //  6 bytes
    ImuSample    imu;     // 28 bytes
    PpgSample    ppg;     // 14 bytes
    EdgeResult   edge;    //  2 bytes
};                        // Total: 50 bytes ✓ (jauh di bawah 250 bytes)

// ---------------------------------------------------------------------------
// Heartbeat — dikirim periodik untuk deteksi node mati
// ---------------------------------------------------------------------------
struct __attribute__((packed)) HeartbeatPacket {
    PacketHeader header;
    uint32_t     uptime_s;  // detik sejak boot
    uint8_t      rssi;      // RSSI ESP-NOW terakhir (optional, 0 jika tidak ada)
};

// ---------------------------------------------------------------------------
// Wrapper union — memudahkan cast dari raw bytes ESP-NOW
// ---------------------------------------------------------------------------
union EspNowPayload {
    uint8_t         raw[250];
    ImuPacket       imu;
    PpgPacket       ppg;
    CombinedPacket  combined;   // NEW
    HeartbeatPacket heartbeat;

    // Helper: baca tipe paket tanpa cast penuh
    PacketType type() const { return static_cast<PacketType>(raw[0]); }
};

// ---------------------------------------------------------------------------
// Pesan internal antar FreeRTOS task di gateway (disimpan dalam queue)
// Payload diperbesar untuk akomodasi JSON combined packet.
// ---------------------------------------------------------------------------
struct MqttMessage {
    char topic[80];
    char payload[400]; // diperbesar dari 200 → 400 untuk combined JSON
};