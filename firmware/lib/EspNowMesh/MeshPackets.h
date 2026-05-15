// File: firmware/lib/EspNowMesh/MeshPackets.h

#pragma once
// =============================================================================
// MeshPackets.h — Satu-satunya definisi struct paket ESP-NOW
// =============================================================================
//
// PERUBAHAN v2 (refactor):
//   - MqttMessage dipindah ke sini (sebelumnya di DataModels.h)
//   - RawPacket dipindah ke sini
//   - DataModels.h lama (root include/ dan firmware/include/) sudah TIDAK DIPAKAI
//     Hapus atau kosongkan kedua file tersebut setelah migrasi selesai.
//
// File ini adalah SATU-SATUNYA sumber kebenaran untuk:
//   1. Struct paket ESP-NOW (CS1AxisPacket, CSPpgPacket, CombinedPacket, dst.)
//   2. MqttMessage — pesan internal antar FreeRTOS task di gateway
//   3. RawPacket   — wrapper ISR → taskMeshHandler
//
// Semua nama field menggunakan camelCase (nodeId, accelX, fingerOn, dst.)
// agar konsisten dengan gaya kode baru. MeshRouting.cpp sudah diupdate
// untuk memakai nama field baru ini.
//
// BATAS UKURAN ESP-NOW: 250 bytes per frame.
// Layout ukuran (verify sebelum deploy):
//   PacketHeader    =   6 bytes
//   CS1AxisPacket   = 136 bytes  ✓ (6 + 32×4 + 2)
//   CSPpgPacket     = 138 bytes  ✓ (6 + 32×4 + 1 + 1 + 2)
//   CombinedPacket  =  50 bytes  ✓ (6 + 28 + 14 + 2)
//   HeartbeatPacket =  11 bytes  ✓
// =============================================================================

#include <Arduino.h>
#include "CS_Sensor.h"  // CS_N, CS_M


// =============================================================================
// PacketType — byte pertama setiap frame, dipakai untuk routing di gateway
// =============================================================================
enum class PacketType : uint8_t
{
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

// Alias uint8_t untuk switch-case tanpa cast berulang
static constexpr uint8_t PKT_CS_AX  = static_cast<uint8_t>(PacketType::CS_AX);
static constexpr uint8_t PKT_CS_AY  = static_cast<uint8_t>(PacketType::CS_AY);
static constexpr uint8_t PKT_CS_AZ  = static_cast<uint8_t>(PacketType::CS_AZ);
static constexpr uint8_t PKT_CS_GX  = static_cast<uint8_t>(PacketType::CS_GX);
static constexpr uint8_t PKT_CS_GY  = static_cast<uint8_t>(PacketType::CS_GY);
static constexpr uint8_t PKT_CS_GZ  = static_cast<uint8_t>(PacketType::CS_GZ);
static constexpr uint8_t PKT_CS_IR  = static_cast<uint8_t>(PacketType::CS_IR);


// =============================================================================
// Sub-structs — building blocks paket ESP-NOW
//
// CATATAN NAMA FIELD (camelCase):
//   nodeId      ← sebelumnya node_id
//   accelX/Y/Z  ← sebelumnya accel_x/y/z
//   gyroX/Y/Z   ← sebelumnya gyro_x/y/z
//   irRaw       ← sebelumnya ir_raw
//   redRaw      ← sebelumnya red_raw
//   heartRate   ← sebelumnya heart_rate
//   fingerOn    ← sebelumnya finger_on
//   uptimeS     ← sebelumnya uptime_s
//   yIr         ← sebelumnya y_ir
//   ppgValid    ← sebelumnya ppg_valid
// =============================================================================

// Header umum — byte pertama setiap frame
struct __attribute__((packed)) PacketHeader
{
    PacketType type;       // 1 byte
    uint8_t    nodeId;     // 1 byte — ID node pengirim (1, 2, 3, ...)
    uint32_t   timestamp;  // 4 byte — millis() saat data diambil
};                         // Total: 6 bytes

// Data IMU dari MPU6050
struct __attribute__((packed)) ImuSample
{
    float accelX;  // m/s²
    float accelY;
    float accelZ;
    float gyroX;   // °/s
    float gyroY;
    float gyroZ;
    float tempC;   // suhu dari register MPU (jarang dipakai)
};                 // Total: 28 bytes

// Data PPG dari MAX30102
struct __attribute__((packed)) PpgSample
{
    uint32_t irRaw;      // nilai LED inframerah (raw ADC)
    uint32_t redRaw;     // nilai LED merah (raw ADC)
    float    spo2;       // SpO2 hasil kalkulasi (0–100%)
    int8_t   heartRate;  // BPM hasil kalkulasi (-1 jika invalid)
    bool     valid;      // true jika HR/SpO2 dalam range fisiologis
};                       // Total: 14 bytes

// Hasil edge computing di sensor node
struct __attribute__((packed)) EdgeResult
{
    bool    fingerOn;  // true jika IR > IR_FINGER_THRESHOLD
    uint8_t reserved;  // padding untuk alignment
};                     // Total: 2 bytes


// =============================================================================
// Packet Types — frame lengkap yang dikirim via ESP-NOW
// =============================================================================

// CombinedPacket — semua data sensor dalam satu frame (mode non-CS)
struct __attribute__((packed)) CombinedPacket
{
    PacketHeader header;  //  6 bytes
    ImuSample    imu;     // 28 bytes
    PpgSample    ppg;     // 14 bytes
    EdgeResult   edge;    //  2 bytes
};                        // Total: 50 bytes ✓

// HeartbeatPacket — dikirim periodik untuk deteksi node mati
struct __attribute__((packed)) HeartbeatPacket
{
    PacketHeader header;   //  6 bytes
    uint32_t     uptimeS;  //  4 bytes
    uint8_t      rssi;     //  1 byte
};                         // Total: 11 bytes ✓

// CS1AxisPacket — satu sinyal CS (ax/ay/az/gx/gy/gz)
struct __attribute__((packed)) CS1AxisPacket
{
    PacketHeader header;   //   6 bytes — header.type membedakan axis mana
    float        y[CS_M];  // 128 bytes — measurement vector
    EdgeResult   edge;     //   2 bytes
};                         // Total: 136 bytes ✓

// CSPpgPacket — sinyal CS untuk PPG IR beserta metadata HR
struct __attribute__((packed)) CSPpgPacket
{
    PacketHeader header;     //   6 bytes
    float        yIr[CS_M];  // 128 bytes
    int8_t       heartRate;  //   1 byte
    bool         ppgValid;   //   1 byte
    EdgeResult   edge;       //   2 bytes
};                           // Total: 138 bytes ✓

// RawPacket — wrapper internal ISR → taskMeshHandler (tidak dikirim via ESP-NOW)
struct RawPacket
{
    uint8_t data[250];  // raw ESP-NOW payload (max 250 bytes)
    uint8_t len;        // panjang aktual data yang valid
    uint8_t srcMac[6];  // MAC address pengirim
};                      // Total: 257 bytes


// =============================================================================
// MqttMessage — pesan internal antar FreeRTOS task di gateway
//
// Dipindah dari DataModels.h ke sini agar semua definisi ada di satu tempat.
//
// ⚠ PERHATIAN UKURAN RAM:
//   sizeof(MqttMessage) × QueueLen::MQTT_MSG = total heap queue
//   Payload 420 bytes = cukup untuk cs_ir (~360B) + margin 60B
//   RAM queue: 30 × (80 + 420) = 15,000 bytes = 15 KB ← AMAN
//   JANGAN naikkan tanpa hitung ulang!
// =============================================================================
struct MqttMessage
{
    char topic[80];
    char payload[420];
};


// =============================================================================
// EspNowPayload — union untuk cast raw bytes ke struct yang sesuai
// =============================================================================
union EspNowPayload
{
    uint8_t         raw[250];
    CombinedPacket  combined;
    HeartbeatPacket heartbeat;
    CS1AxisPacket   csAxis;
    CSPpgPacket     csPpg;

    PacketType type() const { return static_cast<PacketType>(raw[0]); }
};