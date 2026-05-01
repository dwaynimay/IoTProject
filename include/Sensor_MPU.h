#pragma once
// =============================================================================
// Sensor_MPU.h — MPU6050 Manual via Wire1 (pin 21=SDA, 22=SCL)
// Wire1 dipakai agar tidak konflik dengan MAX30102 di Wire (pin 18/19)
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include "DataModels.h"

namespace Mpu6050Reg {
    constexpr uint8_t ACCEL_XOUT_H = 0x3B;
    constexpr uint8_t PWR_MGMT_1   = 0x6B;
}

class SensorMPU {
public:
    SensorMPU() = default;

    bool begin();
    bool read(ImuSample& out);
    void calibrate(uint16_t samples = 500);
    void setSleep(bool enable);
    bool isConnected() const { return _connected; }

private:
    bool _connected = false;

    // ⚠️ Tipe HARUS int16_t — offset disimpan dalam raw ADC, bukan float
    //    Sensor_MPU.cpp mengoperasikan raw int16 sebelum konversi ke fisik
    int16_t _ax_off = 0, _ay_off = 0, _az_off = 0;
    int16_t _gx_off = 0, _gy_off = 0, _gz_off = 0;

    static constexpr float   ACCEL_SCALE = 16384.0f;
    static constexpr float   GYRO_SCALE  = 131.0f;
    static constexpr float   GRAVITY     = 9.80665f;
};