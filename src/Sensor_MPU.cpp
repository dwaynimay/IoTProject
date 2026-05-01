// =============================================================================
// Sensor_MPU.cpp — MPU6050 via Wire1 (pin SDA=21, SCL=22)
// =============================================================================

#include "Sensor_MPU.h"
#include "Config.h"

bool SensorMPU::begin() {
    Wire1.begin(Pin::MPU_SDA, Pin::MPU_SCL);
    Wire1.setClock(I2CClock::SPEED);
    delay(100);

    Wire1.beginTransmission(I2CAddr::MPU6050);
    Wire1.write(Mpu6050Reg::PWR_MGMT_1);
    Wire1.write(0x00);
    uint8_t err = Wire1.endTransmission();

    if (err != 0) {
        Serial.printf("[MPU] ERROR: tidak ada respons I2C (err=%d).\n", err);
        _connected = false;
        return false;
    }

    // Verifikasi burst read bisa berjalan — sama persis dengan kode test
    Wire1.beginTransmission(I2CAddr::MPU6050);
    Wire1.write(Mpu6050Reg::ACCEL_XOUT_H);
    Wire1.endTransmission(false);
    int received = Wire1.requestFrom((int)I2CAddr::MPU6050, 14); // int, bukan uint8_t
    int avail    = Wire1.available();

    Serial.printf("[MPU] Verifikasi burst read: requestFrom=%d available=%d\n",
                  received, avail);

    // Kosongkan buffer
    while (Wire1.available()) Wire1.read();

    if (avail < 14) {
        Serial.println("[MPU] ERROR: burst read gagal. Sensor tidak merespons dengan benar.");
        _connected = false;
        return false;
    }

    _connected = true;
    Serial.println("[OK] MPU6050 Siap (Wire1, pin 21/22)");
    return true;
}

bool SensorMPU::read(ImuSample& out) {
    if (!_connected) return false;

    // requestFrom dengan int literal — sama persis dengan kode test yang berhasil:
    //   Wire.requestFrom(0x68, 14)  ← int, int
    Wire1.beginTransmission(I2CAddr::MPU6050);
    Wire1.write(Mpu6050Reg::ACCEL_XOUT_H);
    Wire1.endTransmission(false);
    Wire1.requestFrom((int)I2CAddr::MPU6050, 14); // ← cast ke int, bukan uint8_t

    if (Wire1.available() < 14) return false;

    // Parse identik dengan kode test
    int16_t raw_ax = Wire1.read() << 8 | Wire1.read();
    int16_t raw_ay = Wire1.read() << 8 | Wire1.read();
    int16_t raw_az = Wire1.read() << 8 | Wire1.read();
    Wire1.read(); Wire1.read(); // skip temperature
    int16_t raw_gx = Wire1.read() << 8 | Wire1.read();
    int16_t raw_gy = Wire1.read() << 8 | Wire1.read();
    int16_t raw_gz = Wire1.read() << 8 | Wire1.read();

    out.accel_x = ((raw_ax - _ax_off) / ACCEL_SCALE) * GRAVITY;
    out.accel_y = ((raw_ay - _ay_off) / ACCEL_SCALE) * GRAVITY;
    out.accel_z = ((raw_az - _az_off) / ACCEL_SCALE) * GRAVITY;
    out.gyro_x  =  (raw_gx - _gx_off) / GYRO_SCALE;
    out.gyro_y  =  (raw_gy - _gy_off) / GYRO_SCALE;
    out.gyro_z  =  (raw_gz - _gz_off) / GYRO_SCALE;
    out.temp_c  = 0;

    return true;
}

void SensorMPU::calibrate(uint16_t samples) {
    if (!_connected) return;
    Serial.printf("[MPU] Kalibrasi %d sampel...\n", samples);

    int32_t sum_ax=0, sum_ay=0, sum_az=0;
    int32_t sum_gx=0, sum_gy=0, sum_gz=0;
    uint16_t valid = 0;

    for (uint16_t i = 0; i < samples; i++) {
        Wire1.beginTransmission(I2CAddr::MPU6050);
        Wire1.write(Mpu6050Reg::ACCEL_XOUT_H);
        Wire1.endTransmission(false);
        Wire1.requestFrom((int)I2CAddr::MPU6050, 14);

        if (Wire1.available() >= 14) {
            int16_t ax = Wire1.read()<<8|Wire1.read();
            int16_t ay = Wire1.read()<<8|Wire1.read();
            int16_t az = Wire1.read()<<8|Wire1.read();
            Wire1.read(); Wire1.read();
            int16_t gx = Wire1.read()<<8|Wire1.read();
            int16_t gy = Wire1.read()<<8|Wire1.read();
            int16_t gz = Wire1.read()<<8|Wire1.read();
            sum_ax+=ax; sum_ay+=ay; sum_az+=az;
            sum_gx+=gx; sum_gy+=gy; sum_gz+=gz;
            valid++;
        }
        delay(5);
    }

    if (valid == 0) { Serial.println("[MPU] Kalibrasi gagal."); return; }

    _ax_off = sum_ax/valid;
    _ay_off = sum_ay/valid;
    _az_off = (sum_az/valid) - (int16_t)ACCEL_SCALE;
    _gx_off = sum_gx/valid;
    _gy_off = sum_gy/valid;
    _gz_off = sum_gz/valid;

    Serial.printf("[MPU] Kalibrasi OK. ax:%d ay:%d az:%d gx:%d gy:%d gz:%d\n",
                  _ax_off,_ay_off,_az_off,_gx_off,_gy_off,_gz_off);
}

void SensorMPU::setSleep(bool enable) {
    if (!_connected) return;
    Wire1.beginTransmission(I2CAddr::MPU6050);
    Wire1.write(Mpu6050Reg::PWR_MGMT_1);
    Wire1.write(enable ? 0x40 : 0x00);
    Wire1.endTransmission();
}