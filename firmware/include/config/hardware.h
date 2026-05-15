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
// Sistem ini memakai DUA bus I2C terpisah agar MPU6050 dan MAX30102
// tidak saling mengganggu (terutama pada sensor kloningan/KW).
//
//   Wire  (bus 1) → MAX30102 PPG   : pin 18 (SDA) & 19 (SCL)
//   Wire1 (bus 2) → MPU6050 IMU   : pin 21 (SDA) & 22 (SCL)
//
// Jika ingin menyatukan ke satu bus, pastikan sensor Anda kompatibel.
// ---------------------------------------------------------------------------
namespace Pin
{
    // Bus 1 — MAX30102 (PPG / Heart Rate)
    constexpr uint8_t PPG_SDA = 18;
    constexpr uint8_t PPG_SCL = 19;
    constexpr uint8_t PPG_INT = 23; // Interrupt MAX30102 (aktif-low, open-drain)

    // Bus 2 — MPU6050 (Accelerometer + Gyroscope)
    constexpr uint8_t MPU_SDA = 21;
    constexpr uint8_t MPU_SCL = 22;
}

// ---------------------------------------------------------------------------
// I2C Clock Speed
//
// Dikunci di 100 kHz karena sensor MPU6050 kloningan sering tidak stabil
// di Fast Mode (400 kHz). Jangan naikkan kecuali sensor Anda original.
// ---------------------------------------------------------------------------
namespace I2CClock
{
    constexpr uint32_t SPEED = 100000UL; // 100 kHz (Standard Mode)
}

// ---------------------------------------------------------------------------
// Alamat I2C (tidak perlu diubah kecuali sensor Anda berbeda)
// ---------------------------------------------------------------------------
namespace I2CAddr
{
    constexpr uint8_t MPU6050  = 0x68; // AD0 pin → GND
    constexpr uint8_t MAX30102 = 0x57;
}

// ---------------------------------------------------------------------------
// MAC Address ESP32
//
// CARA CEK MAC ADDRESS:
//   Upload sketch sederhana, buka Serial Monitor:
//     #include <WiFi.h>
//     void setup() { Serial.begin(115200); WiFi.mode(WIFI_STA); Serial.println(WiFi.macAddress()); }
//     void loop() {}
//
// Ganti nilai di bawah dengan MAC address ESP32 Anda masing-masing.
// ---------------------------------------------------------------------------
namespace MacAddr
{
    // Node A (Sensor 1) — cek log boot: "[ESP-NOW] MAC lokal: XX:XX:XX:XX:XX:XX"
    constexpr uint8_t NODE_A[6]  = {0xF4, 0x2D, 0xC9, 0x6F, 0x5C, 0x40};

    // Node B (Sensor 2) — ganti dengan MAC ESP32 kedua Anda
    constexpr uint8_t NODE_B[6]  = {0x28, 0x05, 0xA5, 0x31, 0xF4, 0x94};

    // Node C (Gateway) — ganti dengan MAC ESP32 gateway Anda
    constexpr uint8_t GATEWAY[6] = {0xF4, 0x2D, 0xC9, 0x70, 0xD1, 0x34};
}
