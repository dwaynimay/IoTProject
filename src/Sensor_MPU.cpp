// =============================================================================
// Sensor_MPU.cpp — MPU6050 Manual (kode berhasil → dijadikan class)
// =============================================================================

#include "Sensor_MPU.h"
#include "Config.h"

// ---------------------------------------------------------------------------
bool SensorMPU::begin() {
    // Wire.begin dengan pin yang sama persis dengan kode yang berhasil
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2CClock::SPEED); // 100 kHz

    delay(100); // tunggu sensor power-on

    // Wake up MPU6050 — IDENTIK dengan kode asli yang berhasil:
    //   Wire.beginTransmission(0x68);
    //   Wire.write(0x6B);
    //   Wire.write(0x00);
    //   Wire.endTransmission();
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1); // 0x6B
    Wire.write(0x00);                   // wake up
    uint8_t err = Wire.endTransmission();

    if (err != 0) {
        Serial.printf("[MPU] ERROR: tidak ada respons I2C (err=%d). Cek kabel SDA/SCL.\n", err);
        _connected = false;
        return false;
    }

    _connected = true;
    Serial.println("[OK] MPU6050 Siap (Manual Mode)");
    return true;
}

// ---------------------------------------------------------------------------
bool SensorMPU::read(ImuSample& out) {
    if (!_connected) return false;

    // Burst read 14 byte dari 0x3B — IDENTIK dengan kode asli yang berhasil:
    //   Wire.beginTransmission(0x68);
    //   Wire.write(0x3B);
    //   Wire.endTransmission(false);
    //   Wire.requestFrom(0x68, 14);
    //   if (Wire.available() == 14) { ... }
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::ACCEL_XOUT_H); // 0x3B
    Wire.endTransmission(false);           // repeated start
    Wire.requestFrom(I2CAddr::MPU6050, static_cast<uint8_t>(14));

    if (Wire.available() < 14) {
        return false;
    }

    // Parse persis seperti kode asli: Wire.read() << 8 | Wire.read()
    int16_t raw_ax = Wire.read() << 8 | Wire.read();
    int16_t raw_ay = Wire.read() << 8 | Wire.read();
    int16_t raw_az = Wire.read() << 8 | Wire.read();

    Wire.read(); Wire.read(); // skip temperature (sesuai kode asli)

    int16_t raw_gx = Wire.read() << 8 | Wire.read();
    int16_t raw_gy = Wire.read() << 8 | Wire.read();
    int16_t raw_gz = Wire.read() << 8 | Wire.read();

    // Kurangi offset kalibrasi (raw) lalu konversi ke satuan fisik
    out.accel_x = ((raw_ax - _ax_off) / ACCEL_SCALE) * GRAVITY;
    out.accel_y = ((raw_ay - _ay_off) / ACCEL_SCALE) * GRAVITY;
    out.accel_z = ((raw_az - _az_off) / ACCEL_SCALE) * GRAVITY;
    out.gyro_x  =  (raw_gx - _gx_off) / GYRO_SCALE;
    out.gyro_y  =  (raw_gy - _gy_off) / GYRO_SCALE;
    out.gyro_z  =  (raw_gz - _gz_off) / GYRO_SCALE;
    out.temp_c  = 0; // tidak dibaca (skip di kode asli)

    return true;
}

// ---------------------------------------------------------------------------
void SensorMPU::calibrate(uint16_t samples) {
    if (!_connected) return;

    Serial.printf("[MPU] Kalibrasi %d sampel — jangan gerakkan sensor...\n", samples);

    int32_t sum_ax = 0, sum_ay = 0, sum_az = 0;
    int32_t sum_gx = 0, sum_gy = 0, sum_gz = 0;
    uint16_t valid = 0;

    for (uint16_t i = 0; i < samples; i++) {
        Wire.beginTransmission(I2CAddr::MPU6050);
        Wire.write(Mpu6050Reg::ACCEL_XOUT_H);
        Wire.endTransmission(false);
        Wire.requestFrom(I2CAddr::MPU6050, static_cast<uint8_t>(14));

        if (Wire.available() >= 14) {
            int16_t ax = Wire.read() << 8 | Wire.read();
            int16_t ay = Wire.read() << 8 | Wire.read();
            int16_t az = Wire.read() << 8 | Wire.read();
            Wire.read(); Wire.read(); // skip temp
            int16_t gx = Wire.read() << 8 | Wire.read();
            int16_t gy = Wire.read() << 8 | Wire.read();
            int16_t gz = Wire.read() << 8 | Wire.read();

            sum_ax += ax; sum_ay += ay; sum_az += az;
            sum_gx += gx; sum_gy += gy; sum_gz += gz;
            valid++;
        }
        delay(5);
    }

    if (valid == 0) {
        Serial.println("[MPU] ERROR: kalibrasi gagal.");
        return;
    }

    _ax_off = sum_ax / valid;
    _ay_off = sum_ay / valid;
    // Target az = +1g = +16384 LSB (sensor menghadap atas)
    _az_off = (sum_az / valid) - static_cast<int16_t>(ACCEL_SCALE);
    _gx_off = sum_gx / valid;
    _gy_off = sum_gy / valid;
    _gz_off = sum_gz / valid;

    Serial.printf("[MPU] Kalibrasi OK. Offset raw → ax:%d ay:%d az:%d | gx:%d gy:%d gz:%d\n",
                  _ax_off, _ay_off, _az_off, _gx_off, _gy_off, _gz_off);
}

// ---------------------------------------------------------------------------
void SensorMPU::setSleep(bool enable) {
    if (!_connected) return;
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1);
    Wire.write(enable ? 0x40 : 0x00); // bit6=1: sleep, bit6=0: wake
    Wire.endTransmission();
}