#pragma once
// =============================================================================
// DataModels.h — Definisi Struct Payload ESP-NOW & Data Internal
// Semua struct HARUS packed agar ukuran di pengirim == penerima.
// ESP-NOW max payload: 250 bytes.
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Tipe paket — byte pertama setiap payload, dipakai untuk routing di gateway
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    IMU_DATA = 0x01,
    PPG_DATA = 0x02,
    HEARTBEAT = 0xFF,
};

// ---------------------------------------------------------------------------
// Header umum — ada di setiap paket
// ---------------------------------------------------------------------------
struct __attribute__((packed)) PacketHeader {
    PacketType type;       // jenis paket
    uint8_t    node_id;    // ID node pengirim (1, 2, 3, ...)
    uint32_t   timestamp;  // millis() saat data diambil
};

// ---------------------------------------------------------------------------
// Data IMU dari MPU6050
// ---------------------------------------------------------------------------
struct __attribute__((packed)) ImuSample {
    float accel_x;   // m/s²
    float accel_y;
    float accel_z;
    float gyro_x;    // °/s
    float gyro_y;
    float gyro_z;
    float temp_c;    // suhu dari sensor MPU (bukan suhu lingkungan)
};

struct __attribute__((packed)) ImuPacket {
    PacketHeader header;
    ImuSample    data;
    // Total: 3 + (7 * 4) = 31 bytes  ✓ jauh di bawah 250 bytes
};

// ---------------------------------------------------------------------------
// Data PPG dari MAX30102
// ---------------------------------------------------------------------------
struct __attribute__((packed)) PpgSample {
    uint32_t red_raw;    // nilai LED merah
    uint32_t ir_raw;     // nilai LED inframerah
    float    spo2;       // SpO2 hasil kalkulasi library (0–100%)
    int8_t   heart_rate; // BPM hasil kalkulasi library (-1 jika invalid)
    bool     valid;      // apakah pembacaan HR/SpO2 valid
};

struct __attribute__((packed)) PpgPacket {
    PacketHeader header;
    PpgSample    data;
    // Total: 3 + (4+4+4+1+1) = 17 bytes  ✓
};

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
    uint8_t      raw[250];
    ImuPacket    imu;
    PpgPacket    ppg;
    HeartbeatPacket heartbeat;

    // Helper: baca tipe paket tanpa cast penuh
    PacketType type() const { return raw[0] ? static_cast<PacketType>(raw[0]) : PacketType::HEARTBEAT; }
};

// ---------------------------------------------------------------------------
// Pesan internal antar FreeRTOS task di gateway (disimpan dalam queue)
// ---------------------------------------------------------------------------
struct MqttMessage {
    char topic[80];
    char payload[200];
};