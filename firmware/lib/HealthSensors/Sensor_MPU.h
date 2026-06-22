// File: firmware/lib/HealthSensors/Sensor_MPU.h

#pragma once
// =============================================================================
// Sensor_MPU.h — Driver MPU6050 (Accelerometer + Gyroscope)
// =============================================================================
//
// Hardware:
//   Sensor : MPU6050 (termasuk varian KW/clone)
//   Bus    : Wire (bus I2C utama, pin dikonfigurasi via hardware.h)
//   Catatan: Berjalan di ESP32 terpisah (Node IMU), sehingga tidak konflik dengan PPG.
//
// Kenapa implementasi manual (tanpa library)?
//   Library MPU6050 umum tidak kompatibel dengan sensor kloningan.
//   Implementasi raw I2C register lebih robust untuk hardware KW.
//
// CARA PAKAI:
//   SensorMPU imu;
//   imu.begin();              // inisialisasi & verifikasi koneksi
//   imu.calibrate();          // opsional, kurangi offset bias
//
//   ImuMeasurement data;
//   if (imu.read(data)) {     // baca satu sampel
//       // gunakan data.accelX, data.gyroY, dsb.
//   }
//
// THREAD SAFETY:
//   Tidak thread-safe secara bawaan.
//   Gunakan mutex (misal g_wireMutex) di luar modul ini jika diakses dari beberapa task.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>

// ── Tipe data khusus layer driver ───────────────────────────────────────────
struct ImuMeasurement {
    float accelX = 0, accelY = 0, accelZ = 0; // m/s²
    float gyroX = 0, gyroY = 0, gyroZ = 0;    // °/s
    float tempC = 0;
};


// =============================================================================
// Register Map MPU6050 yang Digunakan
// =============================================================================
namespace Mpu6050Reg
{
    constexpr uint8_t PWR_MGMT_1    = 0x6B; // power management — tulis 0x00 untuk wake
    constexpr uint8_t CONFIG        = 0x1A; // Digital Low Pass Filter (DLPF) configuration
    constexpr uint8_t DLPF_CFG_21HZ = 0x04; // 21 Hz bandwidth
    constexpr uint8_t ACCEL_XOUT_H  = 0x3B; // awal burst read 14 byte (accel + temp + gyro)
}


// =============================================================================
// SensorMPU
// =============================================================================
class SensorMPU
{
public:
    SensorMPU() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Inisialisasi Wire1, wake sensor, dan verifikasi burst read.
    // Kembalikan true jika sensor merespons dengan benar.
    bool begin();

    // ── Data API ──────────────────────────────────────────────────────────────

    // Baca satu sampel IMU ke dalam `out`.
    // Kembalikan false jika Wire (I2C) tidak mengembalikan 14 byte.
    // Nilai sudah dikonversi ke satuan fisik (m/s² dan °/s).
    bool read(ImuMeasurement& out);

    // ── Kalibrasi & NVS ───────────────────────────────────────────────────────

    // Hitung offset bias rata-rata dari `samples` pembacaan.
    // Letakkan sensor dalam posisi datar & diam saat memanggil ini.
    // Offset otomatis disimpan ke NVS (Preferences) setelah selesai.
    void calibrate(uint16_t samples = 500);

    // Muat offset kalibrasi dari NVS (Preferences).
    // Kembalikan true jika data kalibrasi ditemukan, false jika kosong.
    bool loadCalibration();

    // Hapus data kalibrasi dari NVS (kembali ke default pabrik / offset 0).
    void clearCalibration();

    // ── Power Management ──────────────────────────────────────────────────────

    // Toggle sleep mode sensor. enable=true → sensor tidur (hemat daya).
    void setSleep(bool enable);

    // ── Status ────────────────────────────────────────────────────────────────

    bool isConnected() const { return _connected; }

private:
    bool _connected = false;

    // Offset kalibrasi dalam satuan ADC raw (int16_t).
    // Disimpan raw — bukan float — agar operasi pengurangan di read()
    // terjadi sebelum konversi skala, menghindari akumulasi error float.
    int16_t _axOff = 0, _ayOff = 0, _azOff = 0;
    int16_t _gxOff = 0, _gyOff = 0, _gzOff = 0;

    // Faktor konversi ADC → satuan fisik
    // Sumber: MPU6050 datasheet, FS_SEL=0 (±2g) dan FS_SEL=0 (±250°/s)
    static constexpr float ACCEL_SCALE = 16384.0f; // LSB/g
    static constexpr float GYRO_SCALE  = 131.0f;   // LSB/(°/s)
    static constexpr float GRAVITY     = 9.80665f;  // m/s² per g

    // Helper: baca 14 byte sekaligus dari register ACCEL_XOUT_H.
    // Return false jika Wire.available() < 14.
    bool _burstRead(int16_t& ax, int16_t& ay, int16_t& az,
                    int16_t& gx, int16_t& gy, int16_t& gz);
};