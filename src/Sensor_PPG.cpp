// =============================================================================
// Sensor_PPG.cpp — MAX30102 (kode berhasil → dijadikan class)
// =============================================================================

#include "Sensor_PPG.h"
#include "Config.h"
#include <Wire.h>         // WAJIB ditambahkan untuk komunikasi I2C
#include "heartRate.h"    // WAJIB ditambahkan untuk fungsi checkForBeat()

// ---------------------------------------------------------------------------
bool SensorPPG::begin() {
    // Wire sudah di-init oleh SensorMPU::begin() dengan pin & clock yang sama.
    // Langsung panggil sensor.begin() — IDENTIK dengan kode asli:
    //   if (!sensor.begin(Wire, I2C_SPEED_STANDARD)) { ... while(1); }
    if (!_sensor.begin(Wire, I2C_SPEED_STANDARD)) {
        Serial.println("[ERROR] MAX30102 tidak ditemukan! Cek kabel.");
        _connected = false;
        return false;
    }

    // Konfigurasi IDENTIK dengan kode asli yang berhasil:
    _sensor.setup();
    _sensor.setPulseAmplitudeRed(0x0A); // kecil biar stabil
    _sensor.setPulseAmplitudeIR(0x1F);

    _connected = true;
    Serial.println("[OK] MAX30102 Siap");
    return true;
}

// ---------------------------------------------------------------------------
// update() — dipanggil secepat mungkin di task loop (tanpa vTaskDelay di dalam)
// Logika IDENTIK dengan blok MAX30102 di loop() kode asli:
//
//   long irValue = sensor.getIR();
//   if (checkForBeat(irValue)) {
//     long delta = millis() - lastBeat;
//     lastBeat = millis();
//     beatsPerMinute = 60 / (delta / 1000.0);
//     if (bpm < 255 && bpm > 20) {
//       rates[rateSpot++] = bpm;
//       rateSpot %= RATE_SIZE;
//       beatAvg = avg(rates);
//     }
//   }
// ---------------------------------------------------------------------------
void SensorPPG::update() {
    if (!_connected) return;

    _lastIrValue = _sensor.getIR();

    if (checkForBeat(_lastIrValue)) {
        long delta   = millis() - _lastBeat;
        _lastBeat    = millis();
        _beatsPerMinute = 60.0f / (delta / 1000.0f);

        if (_beatsPerMinute < 255 && _beatsPerMinute > 20) {
            _rates[_rateSpot++] = static_cast<byte>(_beatsPerMinute);
            _rateSpot %= RATE_SIZE;

            _beatAvg = 0;
            for (byte x = 0; x < RATE_SIZE; x++) {
                _beatAvg += _rates[x];
            }
            _beatAvg /= RATE_SIZE;
        }
    }
}

// ---------------------------------------------------------------------------
bool SensorPPG::read(PpgSample& out) {
    if (!_connected) return false;

    out.ir_raw     = static_cast<uint32_t>(_lastIrValue);
    out.red_raw    = 0;                              // tidak dipakai di kode asli
    out.heart_rate = static_cast<int8_t>(
                         constrain(_beatAvg, 0, 127));
    out.spo2       = 0;                              // tidak dihitung di kode ini
    out.valid      = (_beatAvg > 20 && _beatAvg < 255);

    return true;
}