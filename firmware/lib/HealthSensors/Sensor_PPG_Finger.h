// File: firmware/lib/HealthSensors/Sensor_PPG_Finger.h

#pragma once

#ifdef USE_PPG_FINGER
// =============================================================================
// SensorPPG (Finger) — Driver for MAX30102 PPG sensor (Finger Placement)
// =============================================================================
//
// Hardware  : MAX30102 PPG sensor on Wire (I2C) via SparkFun MAX30105 library.
// Why this implementation:
//             Implements a thin driver layer that passes raw IR/RED samples to
//             the HeartRateMonitor DSP pipeline, separating hardware capture
//             from signal processing.
//             Critical Init: Wire.begin() MUST be called after esp_now_init().
//
// USAGE:
//   SensorPPG ppg;
//   ppg.begin();
//   // loop (call at >= 50 Hz):
//   ppg.update();
//   PpgMeasurement m;
//   ppg.read(m);
//
// THREAD SAFETY:
//   Not thread-safe.
// =============================================================================

#include <Arduino.h>
#include <MAX30105.h>
#include "HeartRateMonitor.h"

// ── Tipe data khusus layer driver ───────────────────────────────────────────
struct PpgMeasurement {
    uint32_t irRaw = 0;
    uint32_t redRaw = 0;
    int8_t   heartRate = -1;
    float    spo2 = 0.0f;
    bool     valid = false;
};


class SensorPPG
{
public:
    SensorPPG() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool begin();
    void update();
    bool read(PpgMeasurement& out);

    // ── Motion compensation via IMU (opsional) ──────────────────────────────────
    // Panggil tiap loop SEBELUM update() dengan magnitudo akselerasi dari
    // accelerometer (mis. MPU6050): accelMag = sqrt(ax^2+ay^2+az^2) dalam g.
    // Saat diam ~1.0g. Jika tidak dipakai, motion gate jatuh ke berbasis-PPG.
    void setAccel(float accelMag) { _hr.setAccel(accelMag, millis()); }

    // ── Monitoring untuk Serial Plotter / debugging ─────────────────────────────
    float getAcIr()       const { return _hr.getFilteredSignal(); }
    float getThreshold()  const { return _hr.getThreshold(); }
    float getEnvelope()   const { return _hr.getEnvelope(); }
    float getBpm()        const { return static_cast<float>(_hr.getBpm()); }
    bool  isMotion()      const { return _hr.isMotion(); }
    bool  isSignalLost()  const { return _hr.isSignalLost(); }
    bool  isImuMotion()   const { return _hr.isImuMotion(); }
    float getImuDynamic() const { return _hr.getImuDynamic(); }
    bool  hasFinger()     const { return _contact; }

private:
    void setPower(bool enable);
    bool isConnected() const { return _connected; }

    // ── SpO2 ────────────────────────────────────────────────────────────────────
    void  _updateSpo2(long ir, long red, bool beatJustDetected);
    static bool  _acdc(const int32_t* buf, uint8_t len, float& out_ac, float& out_dc);
    static float _rToSpo2(float R);

private:
    MAX30105 _sensor;
    bool     _connected = false;
    bool     _contact   = false;   // kulit menempel?

    long     _lastIr  = 0;
    long     _lastRed = 0;

    // ── Pipeline detak jantung (semua DSP ada di sini) ──────────────────────────
    HeartRateMonitor _hr;

    // =========================================================================
    // SpO2 state (Ratio-of-Ratios)
    // =========================================================================
    static constexpr uint8_t SPO2_FIFO_SIZE = 50;    // ~1 detik @ 50Hz
    static constexpr float   SPO2_VALID_MIN = 70.0f;
    static constexpr float   SPO2_VALID_MAX = 100.0f;
    static constexpr uint8_t SPO2_MIN_BEATS = 3;
    static constexpr uint8_t SPO2_BEAT_AVG  = 4;

    int32_t  _irBuf[SPO2_FIFO_SIZE]  = {};
    int32_t  _redBuf[SPO2_FIFO_SIZE] = {};
    uint8_t  _bufHead = 0;
    uint8_t  _bufFill = 0;

    float    _rBuf[SPO2_BEAT_AVG] = {};
    uint8_t  _rBufSpot = 0;
    uint8_t  _rBufFill = 0;

    float    _spo2      = 0.0f;
    bool     _spo2Valid = false;

    // ── Contact threshold ───────────────────────────────────────────────────────
    // Diturunkan untuk jari karena LED Power lebih kecil, IR raw juga mengecil.
    static constexpr uint32_t IR_CONTACT_MIN = 10000;
};

#endif // USE_PPG_FINGER