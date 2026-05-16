// File: firmware/lib/EspNowMesh/MeshPackets.h

#pragma once
// =============================================================================
// MeshPackets.h — Definisi struct paket ESP-NOW
// =============================================================================
//
// PERUBAHAN v3.0 (Multi-Hop Dynamic Routing):
//   Packet type baru:
//     BEACON      (0x01) → Gateway broadcast periodik untuk RSSI discovery
//     RSSI_REPORT (0x02) → Node ↔ Node tukar info RSSI ke gateway
//     ROUTED_CS   (0x20) → Wrapper paket CS yang di-relay antar node
//
// ARSITEKTUR ROUTING:
//   Setiap node memutuskan rute per-window:
//     DIRECT : node kirim langsung ke gateway (rssi_self >= rssi_neighbor)
//     RELAYED: node kirim ke neighbor, neighbor forward ke gateway
//
// LAYOUT UKURAN (verify < 250 byte):
//   BeaconPacket    =  7 bytes  ✓
//   RssiReportPacket= 10 bytes  ✓
//   RoutedCsPacket  = 143 bytes ✓ (6 + 1 + 136) atau (6 + 1 + 142)
//   CS1AxisPacket   = 136 bytes ✓
//   CSPpgPacket     = 142 bytes ✓
//   CombinedPacket  =  50 bytes ✓
//   HeartbeatPacket =  11 bytes ✓
// =============================================================================

#include <Arduino.h>
#include "CS_Sensor.h"  // CS_N, CS_M


// =============================================================================
// PacketType
// =============================================================================
enum class PacketType : uint8_t
{
    // ── Routing & Discovery ───────────────────────────────────────────────────
    BEACON          = 0x01,  // Gateway → broadcast (RSSI anchor)
    RSSI_REPORT     = 0x02,  // Node → Node (tukar info RSSI ke gateway)

    // ── Data (existing) ───────────────────────────────────────────────────────
    COMBINED_DATA   = 0x03,

    // ── Compressive Sensing ───────────────────────────────────────────────────
    CS_AX           = 0x10,
    CS_AY           = 0x11,
    CS_AZ           = 0x12,
    CS_GX           = 0x13,
    CS_GY           = 0x14,
    CS_GZ           = 0x15,
    CS_IR           = 0x16,

    // ── Multi-Hop Relay ───────────────────────────────────────────────────────
    ROUTED_CS       = 0x20,  // Wrapper: paket CS yang di-forward oleh relay node

    // ── System ────────────────────────────────────────────────────────────────
    HEARTBEAT       = 0xFF,
};

// Shorthand constants untuk CS axis (kompatibilitas kode lama)
static constexpr uint8_t PKT_CS_AX  = static_cast<uint8_t>(PacketType::CS_AX);
static constexpr uint8_t PKT_CS_AY  = static_cast<uint8_t>(PacketType::CS_AY);
static constexpr uint8_t PKT_CS_AZ  = static_cast<uint8_t>(PacketType::CS_AZ);
static constexpr uint8_t PKT_CS_GX  = static_cast<uint8_t>(PacketType::CS_GX);
static constexpr uint8_t PKT_CS_GY  = static_cast<uint8_t>(PacketType::CS_GY);
static constexpr uint8_t PKT_CS_GZ  = static_cast<uint8_t>(PacketType::CS_GZ);
static constexpr uint8_t PKT_CS_IR  = static_cast<uint8_t>(PacketType::CS_IR);


// =============================================================================
// Sub-structs (tidak berubah dari v2)
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
    uint32_t irRaw;
    uint32_t redRaw;
    float    spo2;
    int8_t   heartRate;
    bool     valid;
};  // 14 bytes

struct __attribute__((packed)) EdgeResult
{
    bool    fingerOn;
    uint8_t reserved;
};  // 2 bytes


// =============================================================================
// Routing Packet Types (BARU)
// =============================================================================

// ---------------------------------------------------------------------------
// BeaconPacket — Gateway → broadcast setiap BEACON_INTERVAL_MS
//
// Node yang menerima beacon bisa mengukur RSSI dari paket ini
// menggunakan esp_wifi_80211_rx_cb atau dari metadata recv callback.
// seqNum dipakai untuk deteksi packet loss beacon.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) BeaconPacket
{
    PacketHeader header;   // 6 bytes (nodeId = GATEWAY_NODE_ID = 0)
    uint8_t      seqNum;   // 1 byte  — 0..255 wrap around
};  // Total: 7 bytes ✓

// ---------------------------------------------------------------------------
// RssiReportPacket — Node → Neighbor (tukar info RSSI ke gateway)
//
// Alur:
//   1. Node A terima beacon → catat rssiToGateway
//   2. Node A kirim RssiReportPacket ke Node B berisi rssiToGateway-nya
//   3. Node B simpan rssiToGateway Node A → bisa bandingkan dengan miliknya
//   4. Keduanya tahu siapa yang lebih dekat ke gateway
//
// Nilai RSSI: negatif dalam dBm (misal -45 = kuat, -90 = lemah)
// Kirim sebagai int8_t (range -128..+127, cukup untuk RSSI)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) RssiReportPacket
{
    PacketHeader header;        // 6 bytes (nodeId = pengirim)
    int8_t       rssiToGateway; // 1 byte  — RSSI pengirim ke gateway (dBm)
    uint8_t      hopCount;      // 1 byte  — berapa hop dari gateway (untuk future)
    uint8_t      reserved;      // 1 byte  — padding, selalu 0
};  // Total: 9 bytes ✓

// ---------------------------------------------------------------------------
// RoutedCsPacket — Node → Relay → Gateway
//
// Wrapper untuk CS packet (CS1AxisPacket atau CSPpgPacket) yang dikirim
// melalui relay node. Relay node TIDAK mengubah inner payload — hanya
// membungkus dengan header tambahan lalu forward ke gateway.
//
// Gateway harus membuka wrapper ini dan memproses inner payload
// seolah-olah diterima langsung dari originalNodeId.
//
// Layout:
//   [RoutedHeader 8 byte][inner payload max 142 byte] = max 150 byte ✓
// ---------------------------------------------------------------------------
struct __attribute__((packed)) RoutedCsHeader
{
    PacketType type;            // 1 byte  — selalu ROUTED_CS (0x20)
    uint8_t    relayNodeId;     // 1 byte  — node yang me-relay (A atau B)
    uint8_t    originalNodeId;  // 1 byte  — node sumber data asli
    uint8_t    innerLen;        // 1 byte  — panjang inner payload (byte)
    uint32_t   relayTimestamp;  // 4 bytes — timestamp saat relay terima paket
};  // 8 bytes

// RoutedCsPacket: header (8) + inner payload (max 142) = max 150 byte ✓
struct __attribute__((packed)) RoutedCsPacket
{
    RoutedCsHeader header;
    uint8_t        inner[142];  // inner = CS1AxisPacket atau CSPpgPacket
};  // max 150 bytes ✓


// =============================================================================
// CS Packet Types (tidak berubah dari v2)
// =============================================================================

struct __attribute__((packed)) CombinedPacket
{
    PacketHeader header;
    ImuSample    imu;
    PpgSample    ppg;
    EdgeResult   edge;
};  // 50 bytes ✓

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

struct __attribute__((packed)) CSPpgPacket
{
    PacketHeader header;
    float        yIr[CS_M];
    int8_t       heartRate;
    bool         ppgValid;
    float        spo2;
    EdgeResult   edge;
};  // 142 bytes ✓


// =============================================================================
// Internal structs
// =============================================================================

// RawPacket — wrapper ISR → taskMeshHandler (tidak berubah)
struct RawPacket
{
    uint8_t data[250];
    uint8_t len;
    uint8_t srcMac[6];
};  // 257 bytes

// Buffer akumulasi IMU di gateway (tidak berubah)
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

// MqttMessage — pesan internal antar FreeRTOS task (tidak berubah)
struct MqttMessage
{
    char topic[80];
    char payload[950];
};

// =============================================================================
// EspNowPayload — union cast helper (diperluas untuk packet baru)
// =============================================================================
union EspNowPayload
{
    uint8_t          raw[250];
    BeaconPacket     beacon;
    RssiReportPacket rssiReport;
    RoutedCsPacket   routedCs;
    CombinedPacket   combined;
    HeartbeatPacket  heartbeat;
    CS1AxisPacket    csAxis;
    CSPpgPacket      csPpg;

    PacketType type() const { return static_cast<PacketType>(raw[0]); }
};

// =============================================================================
// Konstanta Routing
// =============================================================================
namespace RoutingCfg
{
    // Node ID khusus untuk gateway
    static constexpr uint8_t GATEWAY_NODE_ID   = 0;

    // Interval gateway kirim beacon (ms)
    static constexpr uint32_t BEACON_INTERVAL_MS = 1000;

    // Durasi discovery phase saat boot sebelum mulai kirim data (ms)
    static constexpr uint32_t DISCOVERY_PHASE_MS = 6000;

    // Interval node tukar RSSI report dengan neighbor (ms)
    static constexpr uint32_t RSSI_EXCHANGE_MS   = 2000;

    // Threshold: pakai relay hanya jika neighbor lebih baik >= N dBm
    // Mencegah flapping saat RSSI hampir sama
    static constexpr int8_t   RELAY_THRESHOLD_DBM = 5;

    // RSSI default saat belum ada pengukuran (sangat lemah)
    static constexpr int8_t   RSSI_UNKNOWN        = -127;

    // Timeout: jika tidak terima beacon/report selama ini, anggap stale
    static constexpr uint32_t RSSI_STALE_MS       = 10000;
}
