// File: firmware/lib/HealthSensors/Sensor_PPG.h

#pragma once
// =============================================================================
// Sensor_PPG.h — Driver MAX30102 (PPG, Heart Rate, SpO2)
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
// ALGORITMA SpO2 — Ratio-of-Ratios:
//   SpO2 dihitung dari rasio AC/DC antara kanal Red dan IR:
//
//     R = (AC_red / DC_red) / (AC_ir / DC_ir)
//
//   AC = amplitudo osilasi pulsatil (peak-to-peak dalam satu window beat)
//   DC = komponen DC (baseline, rata-rata sinyal)
//
//   SpO2 (%) = lookup table R → SpO2
//   Lookup table didasarkan pada kurva empiris Beer-Lambert:
//     SpO2 ≈ 110 − 25 × R   (approx linear fit untuk R ∈ [0.4, 3.4])
//
//   Window kalkulasi: FIFO_SIZE sampel (25 sampel × 40ms = 1 detik)
//   Update rate: setiap kali beat terdeteksi
//
// CARA PAKAI:
//   SensorPPG ppg;
//   ppg.begin();
//
//   // Di dalam task loop (panggil sesering mungkin untuk akurasi HR+SpO2):
//   ppg.update();
//
//   // Ambil hasil (boleh lebih jarang dari update):
//   PpgSample data;
//   ppg.read(data);   // data.spo2 berisi estimasi SpO2 (0 jika invalid)
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

    // Update state internal heart rate + SpO2 — panggil sesering mungkin.
    // Membaca nilai Red + IR terbaru, mendeteksi beat, dan mengakumulasi
    // buffer untuk kalkulasi SpO2.
    void update();

    // Salin state terkini ke `out` (termasuk spo2).
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

    // ── Heart Rate state ──────────────────────────────────────────────────────
    long  _lastIrValue      = 0;
    long  _lastBeatMs       = 0;
    float _beatsPerMinute   = 0.0f;
    int   _beatAvg          = 0;

    static constexpr uint8_t RATE_SIZE = 4;
    byte _rates[RATE_SIZE]  = {};
    byte _rateSpot          = 0;

    // ── SpO2 state ────────────────────────────────────────────────────────────
    //
    // FIFO_SIZE menentukan window kalkulasi SpO2.
    // 25 sampel × polling ~40ms ≈ 1 detik — cukup untuk 1 siklus jantung
    // bahkan di HR rendah (60 BPM = 1 beat/s).
    // Naikkan jika HR sangat rendah atau sinyal noise tinggi.
    static constexpr uint8_t  SPO2_FIFO_SIZE    = 25;

    // Threshold minimum nilai IR untuk dianggap ada jari.
    // Jika IR < threshold, SpO2 tidak dihitung (hasil tidak valid).
    // Sama dengan EdgeConfig::IR_FINGER_THRESHOLD agar konsisten.
    static constexpr uint32_t SPO2_IR_MIN       = 50000;

    // Batas valid SpO2 secara fisiologis (%).
    // Di luar range ini, nilai dianggap artifact.
    static constexpr float    SPO2_VALID_MIN    = 70.0f;
    static constexpr float    SPO2_VALID_MAX    = 100.0f;

    // Jumlah minimum beat yang harus terakumulasi di _spo2Buf
    // sebelum SpO2 dihitung. Mencegah hasil dari 1 beat saja
    // yang masih sangat noisy.
    static constexpr uint8_t  SPO2_MIN_BEATS    = 2;

    // Buffer ring untuk IR dan Red — diisi setiap update()
    int32_t  _irBuf[SPO2_FIFO_SIZE]  = {};
    int32_t  _redBuf[SPO2_FIFO_SIZE] = {};
    uint8_t  _bufHead   = 0;       // index tulis berikutnya (ring buffer)
    uint8_t  _bufFill   = 0;       // jumlah slot terisi (0..SPO2_FIFO_SIZE)

    // Buffer beat-level untuk smoothing SpO2
    // Setiap beat, satu nilai R (ratio) disimpan.
    // SpO2 final = rata-rata dari SPO2_BEAT_AVG nilai R terakhir.
    static constexpr uint8_t  SPO2_BEAT_AVG    = 4;
    float    _rBuf[SPO2_BEAT_AVG] = {};  // buffer nilai R per beat
    uint8_t  _rBufSpot  = 0;
    uint8_t  _rBufFill  = 0;       // berapa beat yang sudah tersimpan

    float    _spo2      = 0.0f;    // hasil SpO2 terakhir yang valid
    bool     _spo2Valid = false;   // true jika _spo2 bisa dipercaya

    // ── SpO2 internal helpers ─────────────────────────────────────────────────

    // Hitung SpO2 dari buffer saat ini.
    // Dipanggil setiap beat terdeteksi.
    void _calcSpo2();

    // Hitung AC (peak-to-peak) dan DC (mean) dari array int32_t.
    // out_ac = max - min, out_dc = mean
    // Kembalikan false jika array kosong atau semua nol.
    static bool _acdc(const int32_t* buf, uint8_t len,
                      float& out_ac, float& out_dc);

    // Konversi nilai R ke estimasi SpO2 via lookup table.
    // R = (AC_red/DC_red) / (AC_ir/DC_ir)
    // Return nilai SpO2 dalam persen (float).
    static float _rToSpo2(float R);

    // ── Hardware config ───────────────────────────────────────────────────────
    static constexpr uint8_t  SAMPLE_RATE     = 100;
    static constexpr uint8_t  SAMPLE_AVG      = 4;
    static constexpr uint8_t  LED_BRIGHTNESS  = 60;
    static constexpr uint16_t PULSE_WIDTH     = 411;
    static constexpr uint16_t ADC_RANGE       = 16384;
};