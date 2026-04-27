// =============================================================================
// Sensor_PPG.cpp — Implementasi MAX30102
// =============================================================================

#include "Sensor_PPG.h"
#include "Config.h"
#include <MAX30105.h>
#include <heartRate.h>
#include <spo2_algorithm.h>

static MAX30105 _ppg;

// Buffer untuk algoritma SpO2 (library SparkFun butuh array 100 elemen)
static constexpr uint8_t BUFFER_LEN = 100;
static uint32_t _irBuffer[BUFFER_LEN]  = {};
static uint32_t _redBuffer[BUFFER_LEN] = {};
static uint8_t  _bufferIdx = 0;
static bool     _bufferFull = false;

// ---------------------------------------------------------------------------
bool SensorPPG::begin() {
    // Wire sudah di-init oleh SensorMPU::begin(), tidak perlu ulang
    if (!_ppg.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("[PPG] ERROR: MAX30102 tidak ditemukan!");
        _connected = false;
        return false;
    }

    _ppg.setup(
        LED_BRIGHTNESS,
        SAMPLE_AVG,
        2,           // ledMode: 2 = Red + IR
        SAMPLE_RATE,
        PULSE_WIDTH,
        ADC_RANGE
    );

    // Pasang interrupt pin sebagai input (active-low, open-drain → butuh pull-up)
    pinMode(Pin::PPG_INT, INPUT_PULLUP);

    _connected = true;
    Serial.println("[PPG] MAX30102 OK");
    return true;
}

// ---------------------------------------------------------------------------
void SensorPPG::update() {
    if (!_connected) return;

    // Periksa FIFO — ambil sampel baru jika interrupt aktif (LOW)
    if (digitalRead(Pin::PPG_INT) == LOW || _ppg.available()) {
        _ppg.check(); // ambil data dari FIFO ke buffer internal library

        while (_ppg.available()) {
            _redBuffer[_bufferIdx] = _ppg.getRed();
            _irBuffer[_bufferIdx]  = _ppg.getIR();
            _red_raw = _redBuffer[_bufferIdx];
            _ir_raw  = _irBuffer[_bufferIdx];
            _ppg.nextSample();

            _bufferIdx++;
            if (_bufferIdx >= BUFFER_LEN) {
                _bufferIdx  = 0;
                _bufferFull = true;
            }
        }

        // Hanya kalkulasi SpO2/HR saat buffer sudah penuh (butuh 100 sampel)
        if (_bufferFull) {
            int32_t spo2_raw;
            int8_t  spo2_valid;
            int32_t hr_raw;
            int8_t  hr_valid;

            maxim_heart_rate_and_oxygen_saturation(
                _irBuffer, BUFFER_LEN,
                _redBuffer,
                &spo2_raw, &spo2_valid,
                &hr_raw,   &hr_valid
            );

            _valid      = (spo2_valid == 1 && hr_valid == 1);
            _spo2       = _valid ? static_cast<float>(spo2_raw) : 0.0f;
            _heart_rate = _valid ? static_cast<int8_t>(constrain(hr_raw, 0, 127)) : -1;
            _newDataReady = true;
        }
    }
}

// ---------------------------------------------------------------------------
bool SensorPPG::read(PpgSample& out) {
    if (!_connected) return false;

    out.red_raw    = _red_raw;
    out.ir_raw     = _ir_raw;
    out.spo2       = _spo2;
    out.heart_rate = _heart_rate;
    out.valid      = _valid;

    _newDataReady = false;
    return true;
}

// ---------------------------------------------------------------------------
void SensorPPG::setPower(bool enable) {
    if (!_connected) return;
    enable ? _ppg.wakeUp() : _ppg.shutDown();
}