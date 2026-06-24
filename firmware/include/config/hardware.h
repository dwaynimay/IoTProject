#pragma once
// =============================================================================
// config/hardware.h — KONFIGURASI HARDWARE
// =============================================================================
// Ubah file ini jika:
//   - Pin I2C berbeda di board Anda
//   - MAC address ESP32 berbeda (cek via Serial: WiFi.macAddress())
//   - Menggunakan sensor lain yang butuh alamat I2C berbeda
// =============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin I2C
//
// Karena sekarang IMU dan PPG berada di Node (ESP32) yang berbeda,
// keduanya dapat menggunakan I2C bus utama (Wire) dengan pin standar.
//
// ---------------------------------------------------------------------------
namespace Pin
{
#if NODE_ROLE == ROLE_SENSOR_PPG
    // PPG menggunakan pin 18 & 19
    constexpr uint8_t I2C_SDA = 18;
    constexpr uint8_t I2C_SCL = 19;
    constexpr uint8_t PPG_INT = 23; // Interrupt MAX30102 (aktif-low, open-drain)
#else
    // IMU menggunakan pin 21 & 22
    constexpr uint8_t I2C_SDA = 21;
    constexpr uint8_t I2C_SCL = 22;
#endif
}

// ---------------------------------------------------------------------------
// I2C Clock Speed
// ---------------------------------------------------------------------------
namespace I2CClock
{
    constexpr uint32_t SPEED = 400000UL; // 400 kHz (Fast Mode)
}

// ---------------------------------------------------------------------------
// Alamat I2C
// ---------------------------------------------------------------------------
namespace I2CAddr
{
    constexpr uint8_t MPU6050  = 0x68; // AD0 pin → GND
    constexpr uint8_t MAX30102 = 0x57; 
}

// ---------------------------------------------------------------------------
// MAC Address ESP32
// ---------------------------------------------------------------------------
namespace MacAddr
{
    constexpr uint8_t NODE_IMU[6]  = {0xF4, 0x2D, 0xC9, 0x6F, 0x5C, 0x40};
    constexpr uint8_t NODE_PPG[6]  = {0x28, 0x05, 0xA5, 0x31, 0xF4, 0x94};
    constexpr uint8_t GATEWAY[6] = {0xF4, 0x2D, 0xC9, 0x70, 0xD1, 0x34};
}

// ---------------------------------------------------------------------------
// PPG Hardware Config
// ---------------------------------------------------------------------------
namespace PpgConfig
{
    constexpr uint8_t  LED_POWER_WRIST = 0x7F;  // ~25.4mA (wrist butuh terang)
    constexpr uint8_t  SAMPLE_AVG      = 4;
    constexpr uint8_t  LED_MODE        = 2;     // Red + IR
    constexpr uint16_t SAMPLE_RATE     = 400;
    constexpr uint16_t PULSE_WIDTH     = 411;
    constexpr uint16_t ADC_RANGE       = 16384;
}
