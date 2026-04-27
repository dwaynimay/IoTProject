// =============================================================================
// Sensor_MPU.cpp — Implementasi MPU6050
// =============================================================================

#include "Sensor_MPU.h"
#include "Config.h"
#include <MPU6050.h>
#include <Wire.h>

// Instance library (static agar tidak expose ke header)
static MPU6050 _mpu(I2CAddr::MPU6050);

// ---------------------------------------------------------------------------
bool SensorMPU::begin() {
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(400000); // Fast mode 400 kHz

    _mpu.initialize();

    if (!_mpu.testConnection()) {
        Serial.println("[MPU] ERROR: Sensor tidak ditemukan di 0x68!");
        _connected = false;
        return false;
    }

    // Konfigurasi range & DLPF
    _mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);   // ±2g
    _mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);    // ±250°/s
    _mpu.setDLPFMode(MPU6050_DLPF_BW_42);              // Low-pass filter 42 Hz

    _connected = true;
    Serial.println("[MPU] Sensor OK (0x68)");
    return true;
}

// ---------------------------------------------------------------------------
bool SensorMPU::read(ImuSample& out) {
    if (!_connected) return false;

    int16_t ax, ay, az, gx, gy, gz;
    _mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

    // Konversi ke satuan fisik + terapkan offset kalibrasi
    out.accel_x = (ax / ACCEL_SCALE * GRAVITY) - _ax_off;
    out.accel_y = (ay / ACCEL_SCALE * GRAVITY) - _ay_off;
    out.accel_z = (az / ACCEL_SCALE * GRAVITY) - _az_off;
    out.gyro_x  = (gx / GYRO_SCALE) - _gx_off;
    out.gyro_y  = (gy / GYRO_SCALE) - _gy_off;
    out.gyro_z  = (gz / GYRO_SCALE) - _gz_off;
    out.temp_c  = _mpu.getTemperature() / 340.0f + 36.53f;

    return true;
}

// ---------------------------------------------------------------------------
void SensorMPU::calibrate(uint16_t samples) {
    if (!_connected) return;

    Serial.printf("[MPU] Kalibrasi dengan %d sampel, jangan gerakkan sensor...\n", samples);

    double sum_ax = 0, sum_ay = 0, sum_az = 0;
    double sum_gx = 0, sum_gy = 0, sum_gz = 0;

    int16_t ax, ay, az, gx, gy, gz;
    for (uint16_t i = 0; i < samples; i++) {
        _mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        sum_ax += ax / ACCEL_SCALE * GRAVITY;
        sum_ay += ay / ACCEL_SCALE * GRAVITY;
        sum_az += az / ACCEL_SCALE * GRAVITY;
        sum_gx += gx / GYRO_SCALE;
        sum_gy += gy / GYRO_SCALE;
        sum_gz += gz / GYRO_SCALE;
        delay(5);
    }

    _ax_off = sum_ax / samples;
    _ay_off = sum_ay / samples;
    // az_off: target = GRAVITY (sensor menghadap atas), jadi offset = avg - g
    _az_off = (sum_az / samples) - GRAVITY;
    _gx_off = sum_gx / samples;
    _gy_off = sum_gy / samples;
    _gz_off = sum_gz / samples;

    Serial.printf("[MPU] Kalibrasi selesai. Offset accel: (%.3f, %.3f, %.3f)\n",
                  _ax_off, _ay_off, _az_off);
    Serial.printf("[MPU]                   Offset gyro:  (%.3f, %.3f, %.3f)\n",
                  _gx_off, _gy_off, _gz_off);
}

// ---------------------------------------------------------------------------
void SensorMPU::setSleep(bool enable) {
    if (!_connected) return;
    _mpu.setSleepEnabled(enable);
}