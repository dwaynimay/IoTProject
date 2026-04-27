#pragma once
// =============================================================================
// Config.h — Konfigurasi Global Proyek
// Satu tempat untuk semua konstanta: pin, kredensial, MAC address, timing.
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Node Role — di-inject oleh platformio.ini via build_flags
// ---------------------------------------------------------------------------
#define ROLE_SENSOR  1
#define ROLE_GATEWAY 2

// ---------------------------------------------------------------------------
// Pin I2C (shared MPU6050 + MAX30102)
// ---------------------------------------------------------------------------
namespace Pin {
    constexpr uint8_t I2C_SDA = 21;
    constexpr uint8_t I2C_SCL = 22;
    constexpr uint8_t PPG_INT = 19;   // MAX30102 interrupt (active-low, open-drain)
}

// ---------------------------------------------------------------------------
// I2C Address
// ---------------------------------------------------------------------------
namespace I2CAddr {
    constexpr uint8_t MPU6050  = 0x68; // AD0 → GND
    constexpr uint8_t MAX30102 = 0x57;
}

// ---------------------------------------------------------------------------
// WiFi & MQTT (hanya dipakai Node Gateway)
// ---------------------------------------------------------------------------
namespace Wifi {
    constexpr char SSID[]     = "YOUR_SSID";
    constexpr char PASSWORD[] = "YOUR_PASSWORD";
}

namespace Mqtt {
    constexpr char BROKER[]       = "192.168.1.100";   // IP broker
    constexpr uint16_t PORT       = 1883;
    constexpr char CLIENT_ID[]    = "esp32_gateway";
    constexpr char USER[]         = "";                 // kosong jika tanpa auth
    constexpr char PASSWORD[]     = "";
    constexpr char TOPIC_BASE[]   = "health_monitor";   // topic: health_monitor/node_1/imu
    constexpr uint16_t KEEPALIVE  = 60;                 // detik
    constexpr uint16_t RECONNECT_DELAY_MS = 5000;
}

// ---------------------------------------------------------------------------
// ESP-NOW MAC Address
// Ganti dengan MAC address aktual tiap ESP32 (lihat via WiFi.macAddress())
// ---------------------------------------------------------------------------
namespace MacAddr {
    // Node A (Sensor 1)
    constexpr uint8_t NODE_A[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    // Node B (Sensor 2)
    constexpr uint8_t NODE_B[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    // Node C (Gateway)
    constexpr uint8_t GATEWAY[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
}

// ---------------------------------------------------------------------------
// Timing & Sampling Rate
// ---------------------------------------------------------------------------
namespace Timing {
    constexpr uint32_t IMU_SAMPLE_MS     = 10;    // 100 Hz
    constexpr uint32_t PPG_SAMPLE_MS     = 20;    // 50 Hz
    constexpr uint32_t ESPNOW_SEND_MS   = 100;   // kirim batch setiap 100ms
    constexpr uint32_t MQTT_PUBLISH_MS  = 500;   // publish ke broker tiap 500ms
    constexpr uint32_t WIFI_TIMEOUT_MS  = 10000; // timeout connect WiFi
}

// ---------------------------------------------------------------------------
// FreeRTOS Task Priority (semakin besar = semakin prioritas)
// ---------------------------------------------------------------------------
namespace TaskPrio {
    constexpr uint8_t SENSOR_IMU  = 3;
    constexpr uint8_t SENSOR_PPG  = 3;
    constexpr uint8_t ESPNOW_TX   = 4;
    constexpr uint8_t ESPNOW_RX   = 4;
    constexpr uint8_t MQTT_PUB    = 2;
}

// ---------------------------------------------------------------------------
// FreeRTOS Stack Size (bytes)
// ---------------------------------------------------------------------------
namespace StackSize {
    constexpr uint32_t SENSOR_IMU  = 4096;
    constexpr uint32_t SENSOR_PPG  = 4096;
    constexpr uint32_t ESPNOW_TX   = 3072;
    constexpr uint32_t ESPNOW_RX   = 3072;
    constexpr uint32_t MQTT_PUB    = 8192;  // lebih besar untuk JSON + TLS (jika pakai)
}

// ---------------------------------------------------------------------------
// Queue / Buffer Size
// ---------------------------------------------------------------------------
namespace QueueLen {
    constexpr uint8_t IMU_DATA  = 10;
    constexpr uint8_t PPG_DATA  = 10;
    constexpr uint8_t MQTT_MSG  = 20;  // antrian pesan masuk di gateway
}