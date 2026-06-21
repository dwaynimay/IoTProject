// File: firmware/lib/HealthSensors/Sensor_PPG.h

#pragma once
// =============================================================================
// Sensor_PPG.h — Driver MAX30102 (PPG, Heart Rate, SpO2) untuk WRIST
// =============================================================================
//
// Arsitektur modular:
//
//   Sensor_PPG          (file ini)   -> hardware I2C + orkestrasi
//     |
//     +-- HeartRateMonitor          -> pipeline detak jantung
//     |     +-- PpgDsp.h            -> komponen DSP reusable
//     |           (BandPass, PeakEnvelope, BeatDetector,
//     |            MotionGate, BpmEstimator)
//     |
//     +-- (SpO2: Ratio-of-Ratios, dihitung internal)
//
// Driver ini sengaja TIPIS: semua logika sinyal ada di modul terpisah yang
// bisa diuji tanpa hardware. Sensor_PPG hanya membaca register sensor dan
// meneruskan sampel ke pipeline.
//
// Hardware:
//   Sensor  : MAX30102          Bus : Wire (SDA=18, SCL=19)
//   Library : SparkFun MAX3010x Posisi : pergelangan tangan
//
// URUTAN INISIALISASI KRITIS:
//   Wire.begin() untuk MAX30102 HARUS dipanggil SETELAH esp_now_init().
//
// CARA PAKAI:
//   SensorPPG ppg;
//   ppg.begin();
//   // loop (panggil secepat mungkin, >=50 Hz):
//   ppg.update();
//   PpgSample s; ppg.read(s);
//
// THREAD SAFETY: tidak thread-safe.
// =============================================================================

#include <Arduino.h>
#include <MAX30105.h>
#include "MeshPackets.h"
#include "HeartRateMonitor.h"


class SensorPPG
{
public:
    SensorPPG() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool begin();
    void update();
    bool read(PpgSample& out);

    // ── Motion compensation via IMU (opsional) ──────────────────────────────────
    // Panggil tiap loop SEBELUM update() dengan magnitudo akselerasi dari
    // accelerometer (mis. MPU6050): accelMag = sqrt(ax^2+ay^2+az^2) dalam g.
    // Saat diam ~1.0g. Jika tidak dipakai, motion gate jatuh ke berbasis-PPG.
    void setAccel(float accelMag) { _hr.setAccel(accelMag, millis()); }

    // ── Monitoring untuk Serial Plotter / debugging ─────────────────────────────
    float getAcIr()       const { return _hr.filteredSignal(); }
    float getThreshold()  const { return _hr.threshold(); }
    float getEnvelope()   const { return _hr.envelope(); }
    float getBpm()        const { return static_cast<float>(_hr.bpm()); }
    bool  inMotion()      const { return _hr.inMotion(); }
    bool  signalLost()    const { return _hr.signalLost(); }
    bool  imuMotion()     const { return _hr.imuMotion(); }
    float imuDynamic()    const { return _hr.imuDynamic(); }
    bool  fingerPresent() const { return _contact; }

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
    static constexpr uint32_t IR_CONTACT_MIN = 30000;

    // ── Hardware config ─────────────────────────────────────────────────────────
    static constexpr uint8_t  LED_POWER_WRIST = 0x7F;  // ~25.4mA (wrist butuh terang)
    static constexpr uint8_t  SAMPLE_AVG      = 4;
    static constexpr uint8_t  LED_MODE        = 2;     // Red + IR
    static constexpr uint16_t SAMPLE_RATE     = 400;
    static constexpr uint16_t PULSE_WIDTH     = 411;
    static constexpr uint16_t ADC_RANGE       = 16384;
};