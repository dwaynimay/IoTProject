// File: firmware/lib/HealthSensors/Sensor_MPU.cpp

// =============================================================================
// Sensor_MPU.cpp — Implementasi Driver MPU6050
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "Sensor_MPU.h"
#include "../../include/Config.h"

static constexpr char TAG[] = "MPU";


// =============================================================================
// begin() — Inisialisasi Sensor
// =============================================================================
bool SensorMPU::begin()
{
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(10); // Kurangi delay awal karena tidak pakai kabel panjang

    // ── Wake sensor dari sleep mode ───────────────────────────────────────────
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1);
    Wire.write(0x00); // 0x00 = wake up (clear sleep bit)
    const uint8_t err = Wire.endTransmission();

    if (err != 0)
    {
        LOG_ERROR(TAG, "Tidak ada respons I2C (err=%d). Cek wiring pin %d/%d",
                  err, Pin::I2C_SDA, Pin::I2C_SCL);
        _connected = false;
        return false;
    }

    // ── Verifikasi burst read ─────────────────────────────────────────────────
    // Pastikan sensor bisa mengembalikan 14 byte sekaligus sebelum dinyatakan OK.
    // Ini penting untuk sensor KW yang kadang ACK tapi tidak bisa burst read.
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::ACCEL_XOUT_H);
    Wire.endTransmission(false); // repeated start — jangan lepas bus

    const int received = Wire.requestFrom((int)I2CAddr::MPU6050, 14);
    const int avail    = Wire.available();

    LOG_DEBUG(TAG, "Verifikasi burst read: requestFrom=%d available=%d", received, avail);

    // Kosongkan buffer — data ini tidak valid (belum dikalibrasi)
    while (Wire.available()) Wire.read();

    if (avail < 14)
    {
        LOG_ERROR(TAG, "Burst read gagal (%d/14 bytes). Sensor KW atau wiring buruk?",
                  avail);
        _connected = false;
        return false;
    }

    _connected = true;
    LOG_INFO(TAG, "MPU6050 siap | Wire pin SDA=%d SCL=%d | clock=%lu Hz",
             Pin::I2C_SDA, Pin::I2C_SCL, I2CClock::SPEED);
    return true;
}


// =============================================================================
// read() — Baca Satu Sampel IMU
// =============================================================================
bool SensorMPU::read(ImuSample& out)
{
    if (!_connected) return false;

    int16_t ax, ay, az, gx, gy, gz;
    if (!_burstRead(ax, ay, az, gx, gy, gz))
        return false;

    // Kurangi offset kalibrasi (dalam domain raw ADC) lalu konversi ke fisik.
    // Urutan: subtract offset → bagi skala → kalikan gravitasi (khusus accel).
    out.accelX = ((ax - _axOff) / ACCEL_SCALE) * GRAVITY;
    out.accelY = ((ay - _ayOff) / ACCEL_SCALE) * GRAVITY;
    out.accelZ = ((az - _azOff) / ACCEL_SCALE) * GRAVITY;
    out.gyroX  =  (gx - _gxOff) / GYRO_SCALE;
    out.gyroY  =  (gy - _gyOff) / GYRO_SCALE;
    out.gyroZ  =  (gz - _gzOff) / GYRO_SCALE;
    out.tempC  = 0; // tidak dipakai — skip parsing register suhu

    return true;
}


// =============================================================================
// calibrate() — Hitung Offset Bias
//
// Rata-ratakan N sampel saat sensor diam untuk mendapat offset tiap axis.
// Sumbu Z accelerometer dikurangi 1g (ACCEL_SCALE) karena saat datar,
// az_raw seharusnya = +16384 (1g gravitasi), bukan 0.
// =============================================================================
void SensorMPU::calibrate(uint16_t samples)
{
    if (!_connected)
    {
        LOG_WARN(TAG, "Kalibrasi dibatalkan — sensor tidak terhubung");
        return;
    }

    LOG_INFO(TAG, "Kalibrasi dimulai (%d sampel) — letakkan sensor dalam posisi datar & diam",
             samples);

    int32_t sumAx = 0, sumAy = 0, sumAz = 0;
    int32_t sumGx = 0, sumGy = 0, sumGz = 0;
    uint16_t valid = 0;

    for (uint16_t i = 0; i < samples; i++)
    {
        int16_t ax, ay, az, gx, gy, gz;
        if (_burstRead(ax, ay, az, gx, gy, gz))
        {
            sumAx += ax; sumAy += ay; sumAz += az;
            sumGx += gx; sumGy += gy; sumGz += gz;
            valid++;
        }
        delay(5); // 5ms antar sampel = ~200Hz
    }

    if (valid == 0)
    {
        LOG_ERROR(TAG, "Kalibrasi gagal — tidak ada sampel valid");
        return;
    }

    _axOff = static_cast<int16_t>(sumAx / valid);
    _ayOff = static_cast<int16_t>(sumAy / valid);
    _azOff = static_cast<int16_t>(sumAz / valid) - static_cast<int16_t>(ACCEL_SCALE);
    _gxOff = static_cast<int16_t>(sumGx / valid);
    _gyOff = static_cast<int16_t>(sumGy / valid);
    _gzOff = static_cast<int16_t>(sumGz / valid);

    LOG_INFO(TAG, "Kalibrasi selesai (%d/%d valid)", valid, samples);
    LOG_DEBUG(TAG, "Offset accel: ax=%d ay=%d az=%d", _axOff, _ayOff, _azOff);
    LOG_DEBUG(TAG, "Offset gyro : gx=%d gy=%d gz=%d", _gxOff, _gyOff, _gzOff);
}


// =============================================================================
// setSleep() — Toggle Power Mode
// =============================================================================
void SensorMPU::setSleep(bool enable)
{
    if (!_connected) return;

    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1);
    Wire.write(enable ? 0x40 : 0x00); // bit 6 = SLEEP
    Wire.endTransmission();

    LOG_DEBUG(TAG, "Sleep mode: %s", enable ? "ON" : "OFF");
}


// =============================================================================
// _burstRead() — Helper Baca 14 Byte Sekaligus
//
// Register layout MPU6050 (burst dari 0x3B):
//   [0-1]  ACCEL_XOUT  [2-3]  ACCEL_YOUT  [4-5]  ACCEL_ZOUT
//   [6-7]  TEMP_OUT    ← di-skip, tidak dipakai
//   [8-9]  GYRO_XOUT   [10-11] GYRO_YOUT  [12-13] GYRO_ZOUT
// =============================================================================
bool SensorMPU::_burstRead(int16_t& ax, int16_t& ay, int16_t& az,
                            int16_t& gx, int16_t& gy, int16_t& gz)
{
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::ACCEL_XOUT_H);
    Wire.endTransmission(false); // repeated start

    Wire.requestFrom((int)I2CAddr::MPU6050, 14);
    if (Wire.available() < 14) return false;

    // High byte dulu, lalu low byte — big-endian sesuai datasheet MPU6050
    ax = Wire.read() << 8 | Wire.read();
    ay = Wire.read() << 8 | Wire.read();
    az = Wire.read() << 8 | Wire.read();

    Wire.read(); Wire.read(); // skip TEMP_OUT (2 byte)

    gx = Wire.read() << 8 | Wire.read();
    gy = Wire.read() << 8 | Wire.read();
    gz = Wire.read() << 8 | Wire.read();

    return true;
}