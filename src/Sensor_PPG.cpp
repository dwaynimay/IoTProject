// =============================================================================
// Sensor_PPG.cpp — MAX30102 via Wire (pin PPG_SDA=18, PPG_SCL=19)
//
// ⚠️ PENTING — kenapa Wire.begin() dipanggil di sini:
//    MPU6050 pakai Wire1 (bus kedua). MAX30102 pakai Wire (bus pertama).
//    Wire.begin() untuk MAX30102 HARUS dipanggil SETELAH esp_now_init().
//    Urutan di main.cpp:
//      1. g_imu.begin()     → Wire1.begin(21,22) — aman, bus berbeda
//      2. g_espnow.begin()  → WiFi.mode + esp_now_init
//      3. g_ppg.begin()     → Wire.begin(18,19)  ← di sini, setelah ESP-NOW
//
//    Jika Wire.begin(18,19) dipanggil SEBELUM esp_now_init(), driver WiFi
//    internal ESP32 bisa mereset state radio saat inisialisasi I2C,
//    menyebabkan channel ESP-NOW kacau → NACK.
// =============================================================================

#include "Sensor_PPG.h"
#include "Config.h"
#include <Wire.h>
#include "heartRate.h"

bool SensorPPG::begin() {
    // Wire (bus pertama) untuk MAX30102, pin PPG_SDA=18, PPG_SCL=19
    // begin() dipanggil di sini — bukan di SensorMPU — karena bus berbeda
    Wire.begin(Pin::PPG_SDA, Pin::PPG_SCL);
    Wire.setClock(I2CClock::SPEED); // 100 kHz, sama dengan Wire1

    delay(100);

    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        if (_sensor.begin(Wire, I2C_SPEED_STANDARD)) break;
        if (attempt == 3) {
            Serial.println("[PPG] ERROR: MAX30102 tidak ditemukan setelah 3x retry.");
            _connected = false;
            return false;
        }
        Serial.printf("[PPG] Retry %d/3...\n", attempt);
        delay(100);
    }

    _sensor.setup();
    _sensor.setPulseAmplitudeRed(0x0A);
    _sensor.setPulseAmplitudeIR(0x1F);

    _connected = true;
    Serial.println("[OK] MAX30102 Siap (Wire, pin 18/19)");
    return true;
}

void SensorPPG::update() {
    if (!_connected) return;

    long irValue = _sensor.getIR();
    if (irValue == 0) return;

    _lastIrValue = irValue;

    if (checkForBeat(_lastIrValue)) {
        long delta      = millis() - _lastBeat;
        _lastBeat       = millis();
        _beatsPerMinute = 60.0f / (delta / 1000.0f);

        if (_beatsPerMinute < 255 && _beatsPerMinute > 20) {
            _rates[_rateSpot++] = static_cast<byte>(_beatsPerMinute);
            _rateSpot %= RATE_SIZE;

            _beatAvg = 0;
            for (byte x = 0; x < RATE_SIZE; x++) _beatAvg += _rates[x];
            _beatAvg /= RATE_SIZE;
        }
    }
}

bool SensorPPG::read(PpgSample& out) {
    if (!_connected) {
        out.ir_raw     = 0;
        out.red_raw    = 0;
        out.heart_rate = -1;
        out.spo2       = 0.0f;
        out.valid      = false;
        return true;
    }

    out.ir_raw     = static_cast<uint32_t>(_lastIrValue);
    out.red_raw    = 0;
    out.heart_rate = static_cast<int8_t>(constrain(_beatAvg, 0, 127));
    out.spo2       = 0.0f;
    out.valid      = (_beatAvg > 20 && _beatAvg < 255);
    return true;
}

void SensorPPG::setPower(bool enable) {
    if (!_connected) return;
    enable ? _sensor.wakeUp() : _sensor.shutDown();
}