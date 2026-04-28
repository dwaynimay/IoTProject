#pragma once
// =============================================================================
// Sensor_MPU.h — Implementasi Manual MPU6050 via Raw I2C Register
//
// ⚠️ Tidak menggunakan library eksternal (sensor KW tidak kompatibel).
//    Semua komunikasi dilakukan langsung ke register MPU6050 via Wire.h.
//
// Referensi register: MPU-6000/6050 Register Map (RM-MPU-6000A-00)
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include "DataModels.h"

// ---------------------------------------------------------------------------
// Peta Register MPU6050 yang dipakai
// ---------------------------------------------------------------------------
namespace Mpu6050Reg {
    constexpr uint8_t SMPLRT_DIV    = 0x19; // Sample rate divider
    constexpr uint8_t CONFIG        = 0x1A; // DLPF & frame sync
    constexpr uint8_t GYRO_CONFIG   = 0x1B; // Full-scale range gyro
    constexpr uint8_t ACCEL_CONFIG  = 0x1C; // Full-scale range accel
    constexpr uint8_t INT_ENABLE    = 0x38; // Interrupt enable
    constexpr uint8_t ACCEL_XOUT_H  = 0x3B; // Awal burst read (14 byte)
    constexpr uint8_t PWR_MGMT_1    = 0x6B; // Power management, wake/sleep
    constexpr uint8_t WHO_AM_I      = 0x75; // Identifikasi chip
}

class SensorMPU {
public:
    SensorMPU() = default;

    // Inisialisasi I2C & wake up sensor.
    // Kembalikan true jika WHO_AM_I terbaca valid.
    bool begin();

    // Baca 14 byte sekaligus via burst read (accel + temp + gyro).
    // Mengisi ImuSample dengan nilai dalam satuan fisik (m/s², °/s, °C).
    // Kembalikan true jika pembacaan berhasil.
    bool read(ImuSample& out);

    // Kalibrasi offset saat sensor diam (~2 detik waktu tunggu).
    void calibrate(uint16_t samples = 500);

    // Masuk/keluar mode sleep (hemat daya)
    void setSleep(bool enable);

    // Apakah sensor terdeteksi
    bool isConnected() const { return _connected; }

private:
    bool _connected = false;

    // Bias kalibrasi dalam satuan fisik
    float _ax_off = 0, _ay_off = 0, _az_off = 0;
    float _gx_off = 0, _gy_off = 0, _gz_off = 0;

    // Faktor konversi — sesuai konfigurasi di begin():
    // Accel ±2g     → 16384 LSB/g
    // Gyro  ±250°/s → 131   LSB/°/s
    static constexpr float ACCEL_SCALE = 16384.0f;
    static constexpr float GYRO_SCALE  = 131.0f;
    static constexpr float GRAVITY     = 9.80665f;

    // Helper I2C low-level
    void    writeReg(uint8_t reg, uint8_t value);
    uint8_t readReg(uint8_t reg);
    bool    readBurst(uint8_t startReg, uint8_t* buf, uint8_t len);

    static inline int16_t toInt16(uint8_t hi, uint8_t lo) {
        return static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
    }
};