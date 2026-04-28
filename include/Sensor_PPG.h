#pragma once
// =============================================================================
// Sensor_PPG.h — Abstraksi MAX30102 (PPG, Heart Rate, SpO2)
// Menggunakan library: SparkFun MAX3010x
// =============================================================================

#include <Arduino.h>
#include <MAX30105.h> // WAJIB ditambahkan untuk objek _sensor
#include "DataModels.h"

class SensorPPG
{
public:
    SensorPPG() = default;

    bool begin();
    void update();
    bool read(PpgSample &out);
    void setPower(bool enable);
    bool isConnected() const { return _connected; }

private:
    MAX30105 _sensor; // Deklarasi objek library SparkFun

    bool _connected = false;
    bool _newDataReady = false;

    // Variabel kalkulasi detak jantung (yang sebelumnya error "not declared")
    long _lastIrValue = 0;
    long _lastBeat = 0;
    float _beatsPerMinute = 0;
    int _beatAvg = 0;

    static const byte RATE_SIZE = 4;
    byte _rates[4] = {0};
    byte _rateSpot = 0;

    // Konstanta konfigurasi sensor
    static constexpr uint8_t SAMPLE_RATE = 100;   // Hz
    static constexpr uint8_t SAMPLE_AVG = 4;      // rata-rata FIFO
    static constexpr uint8_t LED_BRIGHTNESS = 60; // 0–255
    static constexpr uint16_t PULSE_WIDTH = 411;  // µs
    static constexpr uint16_t ADC_RANGE = 16384;
};