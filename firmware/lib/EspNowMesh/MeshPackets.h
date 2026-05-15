// File: firmware/lib/EspNowMesh/MeshPackets.h

#pragma once
// =============================================================================
// MeshPackets.h — Satu-satunya definisi struct paket ESP-NOW
// =============================================================================
//
// PERUBAHAN v2.1 (SpO2):
//   - CSPpgPacket: tambah field spo2 (float, 4 byte)
//     Size baru: 6 + 128 + 1 + 1 + 4 + 2 = 142 byte ✓ (< 250 byte limit)
//   - MqttMessage.payload: tidak perlu diubah (420 byte cukup untuk spo2)
//
// File ini adalah SATU-SATUNYA sumber kebenaran untuk:
//   1. Struct paket ESP-NOW (CS1AxisPacket, CSPpgPacket, CombinedPacket, dst.)
//   2. MqttMessage — pesan internal antar FreeRTOS task di gateway
//   3. RawPacket   — wrapper ISR → taskMeshHandler
//
// BATAS UKURAN ESP-NOW: 250 bytes per frame.
// Layout ukuran (verify sebelum deploy):
//   PacketHeader    =   6 bytes
//   CS1AxisPacket   = 136 bytes  ✓ (6 + 32×4 + 2)
//   CSPpgPacket     = 142 bytes  ✓ (6 + 32×4 + 1 + 1 + 4 + 2)
//   CombinedPacket  =  50 bytes  ✓ (6 + 28 + 14 + 2)
//   HeartbeatPacket =  11 bytes  ✓
// =============================================================================

#include <Arduino.h>
#include "CS_Sensor.h"  // CS_N, CS_M


// =============================================================================
// PacketType
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

static constexpr uint8_t PKT_CS_AX  = static_cast<uint8_t>(PacketType::CS_AX);
static constexpr uint8_t PKT_CS_AY  = static_cast<uint8_t>(PacketType::CS_AY);
static constexpr uint8_t PKT_CS_AZ  = static_cast<uint8_t>(PacketType::CS_AZ);
static constexpr uint8_t PKT_CS_GX  = static_cast<uint8_t>(PacketType::CS_GX);
static constexpr uint8_t PKT_CS_GY  = static_cast<uint8_t>(PacketType::CS_GY);
static constexpr uint8_t PKT_CS_GZ  = static_cast<uint8_t>(PacketType::CS_GZ);
static constexpr uint8_t PKT_CS_IR  = static_cast<uint8_t>(PacketType::CS_IR);


// =============================================================================
// Sub-structs
// =============================================================================

struct __attribute__((packed)) PacketHeader
{
    PacketType type;
    uint8_t    nodeId;
    uint32_t   timestamp;
};  // 6 bytes

struct __attribute__((packed)) ImuSample
{
    float accelX, accelY, accelZ;  // m/s²
    float gyroX,  gyroY,  gyroZ;   // °/s
    float tempC;
};  // 28 bytes

struct __attribute__((packed)) PpgSample
{
    uint32_t irRaw;      // raw ADC inframerah
    uint32_t redRaw;     // raw ADC merah
    float    spo2;       // SpO2 dalam % (0.0 jika tidak valid)
    int8_t   heartRate;  // BPM (-1 jika invalid)
    bool     valid;      // true jika HR dan SpO2 keduanya valid
};  // 14 bytes

struct __attribute__((packed)) EdgeResult
{
    bool    fingerOn;
    uint8_t reserved;
};  // 2 bytes


// =============================================================================
// Packet Types
// =============================================================================

struct __attribute__((packed)) CombinedPacket
{
    PacketHeader header;  //  6 bytes
    ImuSample    imu;     // 28 bytes
    PpgSample    ppg;     // 14 bytes  (termasuk spo2)
    EdgeResult   edge;    //  2 bytes
};  // Total: 50 bytes ✓

struct __attribute__((packed)) HeartbeatPacket
{
    PacketHeader header;
    uint32_t     uptimeS;
    uint8_t      rssi;
};  // 11 bytes ✓

struct __attribute__((packed)) CS1AxisPacket
{
    PacketHeader header;
    float        y[CS_M];
    EdgeResult   edge;
};  // 136 bytes ✓

// ---------------------------------------------------------------------------
// CSPpgPacket — v2.1: tambah spo2 (float, 4 byte)
//
// SpO2 dikirim sebagai metadata bersama sinyal IR terkompresi.
// Nilai 0.0 berarti kalkulasi SpO2 belum valid (jari tidak menempel,
// buffer belum penuh, atau nilai di luar range fisiologis).
//
// Penerima (MeshRouting) harus:
//   - Tampilkan SpO2 hanya jika spo2 > 0.0 && ppgValid == true
//   - 0.0 bukan "SpO2 = 0%" melainkan "SpO2 tidak tersedia"
//
// Size: 6 + 128 + 1 + 1 + 4 + 2 = 142 bytes ✓ (< 250 byte limit)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) CSPpgPacket
{
    PacketHeader header;     //   6 bytes
    float        yIr[CS_M]; // 128 bytes
    int8_t       heartRate;  //   1 byte
    bool         ppgValid;   //   1 byte
    float        spo2;       //   4 bytes  ← BARU
    EdgeResult   edge;       //   2 bytes
};  // Total: 142 bytes ✓

// RawPacket — wrapper ISR → taskMeshHandler
struct RawPacket
{
    uint8_t data[250];
    uint8_t len;
    uint8_t srcMac[6];
};  // 257 bytes

// Buffer akumulasi IMU (internal gateway)
struct ImuWindowBuffer
{
    float    ax[CS_M], ay[CS_M], az[CS_M];
    float    gx[CS_M], gy[CS_M], gz[CS_M];
    uint32_t timestamp;
    bool     fingerOn;
    uint8_t  receivedMask;
    uint8_t  nodeId;
    uint32_t lastUpdateMs;
};

static constexpr uint8_t IMU_ALL_RECEIVED = 0x3F;

// =============================================================================
// MqttMessage
// =============================================================================
struct MqttMessage
{
    char topic[80];
    char payload[950];
};

// =============================================================================
// EspNowPayload — union cast helper
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