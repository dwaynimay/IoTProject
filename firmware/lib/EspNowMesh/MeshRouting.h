// File: firmware/lib/EspNowMesh/MeshRouting.h

#pragma once
// =============================================================================
// MeshRouting.h — Routing Packet ESP-NOW ke MQTT Topic
// =============================================================================
//
// Tanggung jawab modul ini:
//   Menerima RawPacket dari queue ISR, lalu:
//   1. Identifikasi tipe packet dari byte pertama
//   2. Deserialize raw bytes ke struct yang sesuai
//   3. Serialize ke JSON string
//   4. Tentukan MQTT topic tujuan
//   5. Kembalikan MqttMessage siap publish
//
// Kenapa dipisah dari EspNowMesh?
//   EspNowMesh bertanggung jawab atas transport (ESP-NOW protocol).
//   MeshRouting bertanggung jawab atas logika bisnis (packet → MQTT).
//   Pemisahan ini memudahkan unit test dan penambahan packet type baru.
//
// CARA MENAMBAH PACKET TYPE BARU:
//   1. Tambah enum value di MeshPackets.h
//   2. Tambah case di MeshRouting.cpp _routeCs() atau buat fungsi baru
//   3. Tidak perlu mengubah EspNowMesh sama sekali
//
// CARA PAKAI:
//   MqttMessage msg;
//   if (MeshRouting::route(rawPacket, msg)) {
//       // msg.topic dan msg.payload siap di-publish
//   }
// =============================================================================

#include "MeshPackets.h"


enum class RouteResult
{
    PUBLISHED,      // siap publish ke MQTT
    ACCUMULATING,   // sedang akumulasi (normal, bukan error)
    DROPPED,        // paket invalid atau queue penuh
};

// =============================================================================
// MeshRouting — Static Class (tidak perlu instance)
//
// Semua method static karena routing tidak punya state — murni transformasi
// data dari satu format ke format lain.
// =============================================================================
class MeshRouting
{
public:
    // Proses satu RawPacket menjadi MqttMessage.
    // Kembalikan PUBLISHED jika berhasil di-route, ACCUMULATING jika sedang
    // diakumulasi (seperti CS Axis), atau DROPPED jika packet tidak dikenal
    // atau data tidak valid (terlalu pendek, dsb).
    static RouteResult route(const RawPacket& raw, MqttMessage& out);

private:
    // ── Router per Packet Type ────────────────────────────────────────────────
    // Dipanggil oleh route() setelah dispatch berdasarkan PacketType.
    // Setiap fungsi mengisi out.topic dan out.payload, return PUBLISHED/ACCUMULATING/DROPPED.

    static RouteResult _routeCombined (const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeHeartbeat(const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeCsAxis   (const RawPacket& raw, MqttMessage& out);
    static RouteResult _routeCsIr     (const RawPacket& raw, MqttMessage& out);

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Tulis array float y[CS_M] sebagai JSON array ke buffer dst.
    // Kembalikan jumlah byte yang ditulis.
    static int _writeFloatArray(char* dst, int rem,
                                const float* arr, uint8_t len);

    // Nama axis dari PacketType (untuk MQTT topic string)
    static const char* _axisName(uint8_t rawType);

    // Buffer akumulasi IMU — index 0 = node 1, index 1 = node 2
    static ImuWindowBuffer _imuBuf[2];

    static inline uint8_t _nodeIdx(uint8_t nodeId)
    {
        return (nodeId >= 1 && nodeId <= 2) ? (nodeId - 1) : 0;
    }
};