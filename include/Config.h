#pragma once
// =============================================================================
// Config.h — Konfigurasi Global Proyek
// Satu tempat untuk semua konstanta: pin, kredensial, MAC address, timing.
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Node Role — di-inject oleh platformio.ini via build_flags
// ---------------------------------------------------------------------------
#define ROLE_SENSOR 1
#define ROLE_GATEWAY 2

// ---------------------------------------------------------------------------
// Pin I2C (shared MPU6050 + MAX30102)
// ---------------------------------------------------------------------------
namespace Pin
{
    // Jalur 1 (Wire) untuk MPU6050
    constexpr uint8_t MPU_SDA = 21;
    constexpr uint8_t MPU_SCL = 22;

    // Jalur 2 (Wire1) untuk MAX30102 (PPG)
    constexpr uint8_t PPG_SDA = 18;
    constexpr uint8_t PPG_SCL = 19;

    constexpr uint8_t PPG_INT = 23; // Biarkan ini tetap
}
// namespace Pin
// {
//     constexpr uint8_t I2C_SDA = 18;
//     constexpr uint8_t I2C_SCL = 19;
//     constexpr uint8_t PPG_INT = 23; // MAX30102 interrupt (active-low, open-drain)
// }

// ---------------------------------------------------------------------------
// I2C Clock — 100kHz (I2C_SPEED_STANDARD)
// Jangan naikkan ke 400kHz, MPU6050 KW tidak stabil di fast mode
// ---------------------------------------------------------------------------
namespace I2CClock
{
    constexpr uint32_t SPEED = 100000UL; // 100 kHz
}

// ---------------------------------------------------------------------------
// I2C Address
// ---------------------------------------------------------------------------
namespace I2CAddr
{
    constexpr uint8_t MPU6050 = 0x68; // AD0 → GND
    constexpr uint8_t MAX30102 = 0x57;
}

// ---------------------------------------------------------------------------
// WiFi & MQTT (hanya dipakai Node Gateway)
// ---------------------------------------------------------------------------
namespace Wifi
{
    constexpr char SSID[] = "NAMA_WIFI7";
    constexpr char PASSWORD[] = "PASSWORD_WIFI";
}

namespace Mqtt
{
    constexpr char BROKER[] = "192.168.1.18";
    constexpr uint16_t PORT = 1883;
    constexpr char CLIENT_ID[] = "esp32_gateway";
    constexpr char USER[] = "";
    constexpr char PASSWORD[] = "";
    // Topic format: health_monitor/node_<id>/combined
    //               health_monitor/node_<id>/heartbeat
    //               health_monitor/gateway/status
    constexpr char TOPIC_BASE[] = "health_monitor";
    constexpr uint16_t KEEPALIVE = 60;
    constexpr uint16_t RECONNECT_DELAY_MS = 5000;
}

// ---------------------------------------------------------------------------
// ESP-NOW MAC Address
// Ganti dengan MAC address aktual tiap ESP32 (lihat via WiFi.macAddress())
// ---------------------------------------------------------------------------
namespace MacAddr
{
    // Node A (Sensor 1) -> Sesuai log: F4:2D:C9:6F:5C:40
    constexpr uint8_t NODE_A[6] = {0xF4, 0x2D, 0xC9, 0x6F, 0x5C, 0x40};

    // Node B (Sensor 2) -> ganti dengan MAC aktual ESP32 kedua
    constexpr uint8_t NODE_B[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

    // Node C (Gateway) -> Sesuai log: 28:05:A5:31:F4:94
    constexpr uint8_t GATEWAY[6] = {0x28, 0x05, 0xA5, 0x31, 0xF4, 0x94};
}

// ---------------------------------------------------------------------------
// Timing & Throttling
// ---------------------------------------------------------------------------
// Sensor membaca data secepat mungkin di loop internal.
// SEND_INTERVAL_MS mengontrol seberapa sering CombinedPacket dikirim ke gateway.
// Turunkan angka = lebih sering kirim = lebih boros bandwidth & CPU gateway.
// Naikkan angka  = lebih jarang kirim = lebih hemat, latensi lebih tinggi.
//
// Rekomendasi untuk ML server:
//   100ms (10 Hz) — cukup untuk gesture & HR detection
//   200ms ( 5 Hz) — balance antara resolusi & beban
//   500ms ( 2 Hz) — monitoring santai (SpO2, postur)
// ---------------------------------------------------------------------------
namespace Timing
{
    // *** THROTTLING UTAMA — ubah di sini untuk kontrol kecepatan kirim ***
    constexpr uint32_t SEND_INTERVAL_MS = 200; // interval kirim CombinedPacket (ms)

    // Internal sampling — jangan diubah kecuali ada alasan hardware
    constexpr uint32_t PPG_POLL_MS = 0;    // PPG polling: secepat mungkin (0 = taskYIELD)
    constexpr uint32_t IMU_SAMPLE_MS = 10; // IMU di-read tiap 10ms (100Hz internal)

    // Gateway
    constexpr uint32_t MQTT_PUBLISH_MS = 500; // max wait di queue receive
    constexpr uint32_t WIFI_TIMEOUT_MS = 10000;

    // Heartbeat dari sensor ke gateway
    constexpr uint32_t HEARTBEAT_MS = 30000; // 30 detik
}

// ---------------------------------------------------------------------------
// Edge Computing — Finger-On Detection
// Sensor tidak mengirim CombinedPacket jika jari tidak menempel.
// Ini menghemat bandwidth tanpa mengubah data mentah sama sekali.
// Data PPG raw tetap dikirim apa adanya (untuk ML di server).
// ---------------------------------------------------------------------------
namespace EdgeConfig
{
    // Threshold IR untuk deteksi jari menempel di MAX30102.
    // Nilai IR < threshold → dianggap tidak ada jari → paket tidak dikirim.
    // Sesuaikan berdasarkan hardware (coba 50000–100000).
    constexpr uint32_t IR_FINGER_THRESHOLD = 50000;

    // Jika false: paket tetap dikirim meski jari tidak menempel
    //             (berguna saat debug/kalibrasi)
    // Jika true:  paket diblokir ketika finger_on == false
    constexpr bool ENABLE_FINGER_GATE = false;
}

// ---------------------------------------------------------------------------
// Batching Gateway — kumpulkan N sampel, kirim 1 JSON array ke MQTT
//
// BATCHING_ENABLED = true  → kumpulkan BATCH_SIZE sampel per node,
//                             kirim 1 publish berisi JSON array
// BATCHING_ENABLED = false → setiap CombinedPacket langsung di-publish (1:1)
//
// Catatan: Batching mengurangi jumlah MQTT publish calls secara drastis
//          tapi menambah latensi sebesar (BATCH_SIZE × SEND_INTERVAL_MS).
//          Contoh: BATCH_SIZE=5, SEND_INTERVAL=200ms → latensi tambah ~1 detik
// ---------------------------------------------------------------------------
namespace BatchConfig
{
    // *** SWITCH UTAMA — ganti true/false untuk aktifkan/nonaktifkan batching ***
    constexpr bool BATCHING_ENABLED = false; // false = langsung publish (default)
    constexpr uint8_t BATCH_SIZE = 5;        // jumlah sampel per batch (jika enabled)
}

// ---------------------------------------------------------------------------
// FreeRTOS Task Priority (semakin besar = semakin prioritas)
// ---------------------------------------------------------------------------
namespace TaskPrio
{
    constexpr uint8_t SENSOR_PPG = 4; // tertinggi — PPG butuh polling cepat
    constexpr uint8_t SENSOR_IMU = 3;
    constexpr uint8_t ESPNOW_TX = 2;
    constexpr uint8_t MQTT_PUB = 2;
}

// ---------------------------------------------------------------------------
// FreeRTOS Stack Size (bytes)
// ---------------------------------------------------------------------------
namespace StackSize
{
    constexpr uint32_t SENSOR_PPG = 4096;
    constexpr uint32_t SENSOR_IMU = 4096;
    constexpr uint32_t ESPNOW_TX = 3072;
    constexpr uint32_t MQTT_PUB = 8192;
}

// ---------------------------------------------------------------------------
// Queue / Buffer Size
// ---------------------------------------------------------------------------
namespace QueueLen
{
    constexpr uint8_t IMU_DATA = 1;
    constexpr uint8_t PPG_DATA = 1;
    constexpr uint8_t MQTT_MSG = 40; // diperbesar dari 20 → 40 untuk buffer aman
}