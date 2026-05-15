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
#include "DataModels.h"  // MqttMessage


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
    // Kembalikan true jika berhasil di-route, false jika packet tidak dikenal
    // atau data tidak valid (terlalu pendek, dsb).
    static bool route(const RawPacket& raw, MqttMessage& out);

private:
    // ── Router per Packet Type ────────────────────────────────────────────────
    // Dipanggil oleh route() setelah dispatch berdasarkan PacketType.
    // Setiap fungsi mengisi out.topic dan out.payload, return true jika sukses.

    static bool _routeCombined (const RawPacket& raw, MqttMessage& out);
    static bool _routeHeartbeat(const RawPacket& raw, MqttMessage& out);
    static bool _routeCsAxis   (const RawPacket& raw, MqttMessage& out);
    static bool _routeCsIr     (const RawPacket& raw, MqttMessage& out);

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Tulis array float y[CS_M] sebagai JSON array ke buffer dst.
    // Kembalikan jumlah byte yang ditulis.
    static int _writeFloatArray(char* dst, int rem,
                                const float* arr, uint8_t len);

    // Nama axis dari PacketType (untuk MQTT topic string)
    static const char* _axisName(uint8_t rawType);
};