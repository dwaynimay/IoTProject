#pragma once
// =============================================================================
// Sensor_PPG.h — Abstraksi MAX30102 (PPG, Heart Rate, SpO2)
// Menggunakan library: SparkFun MAX3010x
// =============================================================================

#include <Arduino.h>
#include "DataModels.h"

class SensorPPG {
public:
    SensorPPG() = default;

    // Inisialisasi sensor. Kembalikan true jika berhasil.
    // Memasang ISR pada pin interrupt (PPG_INT) jika tersedia.
    bool begin();

    // Update pembacaan internal (panggil sesering mungkin di task loop).
    // Mengambil sampel dari FIFO MAX30102 dan akumulasi untuk kalkulasi HR/SpO2.
    void update();

    // Isi struct PpgSample dengan nilai terbaru. Kembalikan true jika valid.
    bool read(PpgSample& out);

    // Aktifkan/nonaktifkan sensor (low-power off)
    void setPower(bool enable);

    bool isConnected() const { return _connected; }

private:
    bool _connected    = false;
    bool _newDataReady = false;

    // Nilai terakhir
    float    _spo2      = 0.0f;
    int8_t   _heart_rate = -1;
    bool     _valid     = false;
    uint32_t _red_raw   = 0;
    uint32_t _ir_raw    = 0;

    // Konstanta konfigurasi sensor
    static constexpr uint8_t SAMPLE_RATE    = 100;  // Hz
    static constexpr uint8_t SAMPLE_AVG     = 4;    // rata-rata FIFO
    static constexpr uint8_t LED_BRIGHTNESS = 60;   // 0–255 (atur sesuai pengguna)
    static constexpr uint8_t PULSE_WIDTH    = 411;  // µs
    static constexpr uint16_t ADC_RANGE    = 16384;
};