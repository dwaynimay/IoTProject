// File: firmware/lib/HealthSensors/Sensor_PPG.h

#pragma once
// =============================================================================
// Sensor_PPG.h — Driver MAX30102 (PPG, Heart Rate)
// =============================================================================
//
// Hardware:
//   Sensor  : MAX30102 (Pulse Oximeter & Heart Rate)
//   Library : SparkFun MAX3010x
//   Bus     : Wire (I2C bus pertama) — pin SDA=18, SCL=19
//
// ⚠️  URUTAN INISIALISASI KRITIS:
//   Wire.begin() untuk MAX30102 HARUS dipanggil SETELAH esp_now_init().
//   Jika dipanggil sebelumnya, driver WiFi internal ESP32 bisa mereset
//   state radio saat inisialisasi I2C → channel ESP-NOW kacau → NACK.
//
//   Urutan yang benar di setup():
//     1. g_imu.begin()    → Wire1.begin(21, 22)  — bus berbeda, aman
//     2. g_espnow.begin() → WiFi + esp_now_init()
//     3. g_ppg.begin()    → Wire.begin(18, 19)   ← di sini, setelah ESP-NOW
//
// CARA PAKAI:
//   SensorPPG ppg;
//   ppg.begin();
//
//   // Di dalam task loop (panggil sesering mungkin untuk akurasi HR):
//   ppg.update();
//
//   // Ambil hasil (boleh lebih jarang dari update):
//   PpgSample data;
//   ppg.read(data);
//
// THREAD SAFETY:
//   Tidak thread-safe. Gunakan Wire0Mutex di luar modul ini jika
//   update() dan read() dipanggil dari task berbeda.
// =============================================================================

#include <Arduino.h>
#include <MAX30105.h>
#include "MeshPackets.h"


// =============================================================================
// SensorPPG
// =============================================================================
class SensorPPG
{
public:
    SensorPPG() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Inisialisasi Wire, setup MAX30102, retry hingga 3x jika gagal.
    // Kembalikan true jika sensor berhasil diinisialisasi.
    bool begin();

    // ── Data API ──────────────────────────────────────────────────────────────

    // Update state internal heart rate — panggil sesering mungkin di task loop.
    // Membaca nilai IR terbaru dan mendeteksi beat.
    // Tidak mengembalikan nilai — hasil bisa diambil via read().
    void update();

    // Salin state terkini ke `out`.
    // Selalu return true (bahkan saat sensor tidak terhubung, isi out dengan nol).
    bool read(PpgSample& out);

    // ── Power Management ──────────────────────────────────────────────────────

    // Toggle power sensor. enable=false → shutdown (hemat daya).
    void setPower(bool enable);

    // ── Status ────────────────────────────────────────────────────────────────

    bool isConnected() const { return _connected; }

private:
    MAX30105 _sensor;
    bool     _connected = false;

    // State kalkulasi heart rate
    long  _lastIrValue      = 0;
    long  _lastBeatMs       = 0;
    float _beatsPerMinute   = 0.0f;
    int   _beatAvg          = 0;

    // Circular buffer untuk rata-rata BPM
    static constexpr uint8_t RATE_SIZE = 4;
    byte _rates[RATE_SIZE]  = {};
    byte _rateSpot          = 0;

    // Konfigurasi hardware sensor
    static constexpr uint8_t  SAMPLE_RATE     = 100;   // Hz
    static constexpr uint8_t  SAMPLE_AVG      = 4;     // rata-rata FIFO
    static constexpr uint8_t  LED_BRIGHTNESS  = 60;    // 0–255
    static constexpr uint16_t PULSE_WIDTH     = 411;   // µs
    static constexpr uint16_t ADC_RANGE       = 16384;
};