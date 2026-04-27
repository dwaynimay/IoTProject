#pragma once
// =============================================================================
// Sensor_MPU.h — Abstraksi MPU6050 (Accelerometer + Gyroscope)
// Menggunakan library: electroniccats/MPU6050
// =============================================================================

#include <Arduino.h>
#include "DataModels.h"

class SensorMPU {
public:
    SensorMPU() = default;

    // Inisialisasi I2C & sensor. Kembalikan true jika berhasil.
    bool begin();

    // Baca data mentah dan isi struct ImuSample.
    // Kembalikan true jika data valid (tidak timeout, tidak error).
    bool read(ImuSample& out);

    // Kalibrasi offset gyro & accel (panggil saat sensor diam ~2 detik).
    void calibrate(uint16_t samples = 500);

    // Aktifkan/nonaktifkan mode low-power (berguna jika node sleep)
    void setSleep(bool enable);

    // Cek apakah sensor terdeteksi di bus I2C
    bool isConnected() const { return _connected; }

private:
    bool _connected = false;

    // Offset kalibrasi (disimpan setelah calibrate())
    float _ax_off = 0, _ay_off = 0, _az_off = 0;
    float _gx_off = 0, _gy_off = 0, _gz_off = 0;

    // Konversi raw ADC ke satuan fisik
    static constexpr float ACCEL_SCALE = 16384.0f;  // ±2g → 16384 LSB/g
    static constexpr float GYRO_SCALE  = 131.0f;    // ±250°/s → 131 LSB/°/s
    static constexpr float GRAVITY     = 9.80665f;
};