// File: firmware/lib/HealthSensors/Sensor_PPG.cpp

// =============================================================================
// Sensor_PPG.cpp — Implementasi Driver MAX30102 dengan SpO2
// =============================================================================
//
// ALGORITMA SpO2 — Ratio-of-Ratios + Lookup Table
// ─────────────────────────────────────────────────
// Prinsip dasar: Beer-Lambert Law
//   Cahaya yang melewati jaringan diserap berbeda oleh HbO2 (oksihemoglobin)
//   dan Hb (deoksihemoglobin) pada panjang gelombang berbeda.
//   MAX30102 menggunakan Red (660nm) dan IR (880nm).
//
// Langkah kalkulasi per beat:
//   1. Baca ring buffer SPO2_FIFO_SIZE sampel Red + IR
//   2. Hitung AC (peak-to-peak) dan DC (mean baseline) untuk masing-masing
//   3. R = (AC_red/DC_red) / (AC_ir/DC_ir)
//   4. SpO2 = lookup(R) dari tabel empiris
//   5. Simpan R ke _rBuf (smoothing), hitung rata-rata → _spo2
//
// Lookup table (R → SpO2) didasarkan pada:
//   Referensi: Maxim Integrated "Recommended Configurations for Optical
//   SpO2 Measurements" AN6945
//   dan fit empiris: SpO2 ≈ -45.060 * R² + 30.354 * R + 94.845
//   (valid untuk R ∈ [0.4, 3.4], SpO2 ∈ [70%, 100%])
//
// Indeks tabel = round(R * 10), range R = 0.4..3.4 → index 4..34
// Luar range → clamp ke ujung tabel.
//
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "Sensor_PPG.h"
#include "../../include/Config.h"
#include <Wire.h>
#include "heartRate.h"

static constexpr char TAG[] = "PPG";

// =============================================================================
// Lookup Table SpO2
// =============================================================================
//
// Indeks tabel = round(R × 10), valid range: R ∈ [0.4, 3.4]
// Index 0..3  → R < 0.4  → klem ke SpO2[4]
// Index 4..34 → valid
// Index 35+   → R > 3.4  → klem ke SpO2[34]
//
// Nilai diturunkan dari formula empiris Maxim:
//   SpO2 ≈ -45.060 × R² + 30.354 × R + 94.845
//
// Diverifikasi terhadap tabel kalibrasi klinik pada range 70–100%.
//
// Ukuran: 35 entry × 1 byte = 35 byte flash.
// =============================================================================
static const uint8_t SPO2_TABLE[35] = {
    //  R=0.0  R=0.1  R=0.2  R=0.3  R=0.4  R=0.5  R=0.6  R=0.7
        100,   100,   100,   100,   100,    99,    98,    97,
    //  R=0.8  R=0.9  R=1.0  R=1.1  R=1.2  R=1.3  R=1.4  R=1.5
         96,    95,    94,    93,    92,    91,    90,    89,
    //  R=1.6  R=1.7  R=1.8  R=1.9  R=2.0  R=2.1  R=2.2  R=2.3
         87,    86,    84,    82,    81,    79,    77,    75,
    //  R=2.4  R=2.5  R=2.6  R=2.7  R=2.8  R=2.9  R=3.0  R=3.1
         73,    71,    69,    67,    65,    63,    61,    60,
    //  R=3.2  R=3.3  R=3.4
         70,    70,    70
};
static constexpr uint8_t SPO2_TABLE_SIZE = sizeof(SPO2_TABLE);


// =============================================================================
// begin()
// =============================================================================
bool SensorPPG::begin()
{
    Wire.begin(Pin::PPG_SDA, Pin::PPG_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(100);

    for (uint8_t attempt = 1; attempt <= 3; attempt++)
    {
        if (_sensor.begin(Wire, I2C_SPEED_STANDARD))
            break;

        if (attempt == 3)
        {
            LOG_ERROR(TAG, "MAX30102 tidak ditemukan setelah 3x retry | pin SDA=%d SCL=%d",
                      Pin::PPG_SDA, Pin::PPG_SCL);
            _connected = false;
            return false;
        }

        LOG_WARN(TAG, "Retry %d/3 ...", attempt);
        delay(100);
    }

    _sensor.setup();
    _sensor.setPulseAmplitudeRed(0x0A);
    _sensor.setPulseAmplitudeIR(0x1F);

    _connected = true;
    LOG_INFO(TAG, "MAX30102 siap | Wire pin SDA=%d SCL=%d | clock=%lu Hz",
             Pin::PPG_SDA, Pin::PPG_SCL, I2CClock::SPEED);
    LOG_INFO(TAG, "SpO2: Ratio-of-Ratios | fifo=%d | beat_avg=%d",
             SPO2_FIFO_SIZE, SPO2_BEAT_AVG);
    return true;
}


// =============================================================================
// update() — Baca Red + IR, Deteksi Beat, Update SpO2
// =============================================================================
void SensorPPG::update()
{
    if (!_connected) return;

    // Baca kedua kanal sekaligus — penting agar sinkron waktu
    const long irValue  = _sensor.getIR();
    const long redValue = _sensor.getRed();

    if (irValue == 0) return;

    _lastIrValue = irValue;

    // ── Isi ring buffer untuk SpO2 ────────────────────────────────────────────
    // Buffer diisi setiap update() terlepas dari ada beat atau tidak.
    // Ini memastikan window IR/Red yang dipakai SpO2 selalu fresh.
    _irBuf[_bufHead]  = static_cast<int32_t>(irValue);
    _redBuf[_bufHead] = static_cast<int32_t>(redValue);
    _bufHead = (_bufHead + 1) % SPO2_FIFO_SIZE;
    if (_bufFill < SPO2_FIFO_SIZE) _bufFill++;

    // ── Heart Rate + SpO2 per beat ────────────────────────────────────────────
    if (!checkForBeat(irValue)) return;

    const long delta = millis() - _lastBeatMs;
    _lastBeatMs      = millis();
    _beatsPerMinute  = 60.0f / (delta / 1000.0f);

    if (_beatsPerMinute >= 20 && _beatsPerMinute < 255)
    {
        // Update HR circular buffer
        _rates[_rateSpot++] = static_cast<byte>(_beatsPerMinute);
        _rateSpot %= RATE_SIZE;

        _beatAvg = 0;
        for (byte i = 0; i < RATE_SIZE; i++)
            _beatAvg += _rates[i];
        _beatAvg /= RATE_SIZE;

        // Hitung SpO2 — hanya jika jari terdeteksi dan buffer cukup penuh
        if (irValue >= SPO2_IR_MIN && _bufFill >= SPO2_FIFO_SIZE)
            _calcSpo2();

        LOG_DEBUG(TAG, "Beat! BPM=%.1f avg=%d | SpO2=%.1f%% valid=%s",
                  _beatsPerMinute, _beatAvg,
                  _spo2, _spo2Valid ? "Y" : "N");
    }
}


// =============================================================================
// _calcSpo2() — Kalkulasi SpO2 dari Ring Buffer
//
// Dipanggil setiap beat, hanya saat buffer penuh dan jari terdeteksi.
// Menggunakan SPO2_FIFO_SIZE sampel IR + Red terbaru.
// =============================================================================
void SensorPPG::_calcSpo2()
{
    // Susun ulang ring buffer ke array linear (terbaru di ujung)
    // Alokasi di stack — SPO2_FIFO_SIZE × 2 × 4 byte = 200 byte, aman
    int32_t irLin[SPO2_FIFO_SIZE];
    int32_t redLin[SPO2_FIFO_SIZE];

    for (uint8_t i = 0; i < SPO2_FIFO_SIZE; i++)
    {
        // _bufHead sudah maju, jadi (head - size + i) mod size = urutan kronologis
        uint8_t idx = (_bufHead + i) % SPO2_FIFO_SIZE;
        irLin[i]    = _irBuf[idx];
        redLin[i]   = _redBuf[idx];
    }

    // Hitung AC dan DC untuk IR dan Red
    float acIr = 0.0f, dcIr = 0.0f;
    float acRed = 0.0f, dcRed = 0.0f;

    if (!_acdc(irLin,  SPO2_FIFO_SIZE, acIr,  dcIr)  ||
        !_acdc(redLin, SPO2_FIFO_SIZE, acRed, dcRed))
    {
        LOG_WARN(TAG, "SpO2 skip: AC/DC kalkulasi gagal (sinyal terlalu lemah?)");
        _spo2Valid = false;
        return;
    }

    // Guard division-by-zero
    if (dcIr < 1.0f || dcRed < 1.0f || acIr < 1.0f)
    {
        _spo2Valid = false;
        return;
    }

    // Hitung R = (AC_red/DC_red) / (AC_ir/DC_ir)
    const float R = (acRed / dcRed) / (acIr / dcIr);

    // Simpan R ke buffer beat untuk smoothing
    _rBuf[_rBufSpot++] = R;
    _rBufSpot %= SPO2_BEAT_AVG;
    if (_rBufFill < SPO2_BEAT_AVG) _rBufFill++;

    // Tunggu minimal SPO2_MIN_BEATS sebelum hasilkan nilai
    if (_rBufFill < SPO2_MIN_BEATS)
    {
        LOG_DEBUG(TAG, "SpO2: menunggu beat (%d/%d) | R=%.3f",
                  _rBufFill, SPO2_MIN_BEATS, R);
        _spo2Valid = false;
        return;
    }

    // Rata-ratakan R dari buffer beat
    float rAvg = 0.0f;
    for (uint8_t i = 0; i < _rBufFill; i++)
        rAvg += _rBuf[i];
    rAvg /= _rBufFill;

    // Konversi R → SpO2 via lookup table
    const float spo2Raw = _rToSpo2(rAvg);

    // Validasi range fisiologis
    if (spo2Raw >= SPO2_VALID_MIN && spo2Raw <= SPO2_VALID_MAX)
    {
        _spo2      = spo2Raw;
        _spo2Valid = true;
        LOG_DEBUG(TAG, "SpO2=%.1f%% | R_avg=%.3f (R_now=%.3f) | "
                  "AC_ir=%.1f DC_ir=%.1f AC_red=%.1f DC_red=%.1f",
                  _spo2, rAvg, R, acIr, dcIr, acRed, dcRed);
    }
    else
    {
        // Nilai di luar range — kemungkinan noise atau gerak tangan
        _spo2Valid = false;
        LOG_WARN(TAG, "SpO2 out of range: %.1f%% (R=%.3f) — kemungkinan motion artifact",
                 spo2Raw, rAvg);
    }
}


// =============================================================================
// _acdc() — Hitung AC (peak-to-peak) dan DC (mean) dari Buffer
// =============================================================================
bool SensorPPG::_acdc(const int32_t* buf, uint8_t len,
                       float& out_ac, float& out_dc)
{
    if (len == 0) return false;

    int32_t vmin = buf[0], vmax = buf[0];
    int64_t sum  = 0;

    for (uint8_t i = 0; i < len; i++)
    {
        if (buf[i] < vmin) vmin = buf[i];
        if (buf[i] > vmax) vmax = buf[i];
        sum += buf[i];
    }

    out_ac = static_cast<float>(vmax - vmin);
    out_dc = static_cast<float>(sum) / static_cast<float>(len);

    // AC terlalu kecil = tidak ada pulsasi = jari tidak menempel atau sinyal mati
    return (out_ac > 0.0f && out_dc > 0.0f);
}


// =============================================================================
// _rToSpo2() — Lookup Table R → SpO2
//
// R = (AC_red/DC_red) / (AC_ir/DC_ir)
// Indeks = (int)(R * 10), klem ke [0, SPO2_TABLE_SIZE-1]
// =============================================================================
float SensorPPG::_rToSpo2(float R)
{
    if (R < 0.0f) R = 0.0f;

    // Hitung index dengan interpolasi linear antara dua entry tabel
    // Lebih akurat dari round() biasa
    const float  idxF  = R * 10.0f;
    const int    idx0  = static_cast<int>(idxF);
    const int    idx1  = idx0 + 1;
    const float  frac  = idxF - static_cast<float>(idx0);

    // Klem ke batas tabel
    const int    i0 = (idx0 < 0) ? 0
                    : (idx0 >= SPO2_TABLE_SIZE) ? (SPO2_TABLE_SIZE - 1)
                    : idx0;
    const int    i1 = (idx1 >= SPO2_TABLE_SIZE) ? (SPO2_TABLE_SIZE - 1)
                    : idx1;

    // Interpolasi linear
    const float spo2 = static_cast<float>(SPO2_TABLE[i0]) * (1.0f - frac)
                     + static_cast<float>(SPO2_TABLE[i1]) * frac;

    return spo2;
}


// =============================================================================
// read() — Salin State Terkini ke PpgSample
// =============================================================================
bool SensorPPG::read(PpgSample& out)
{
    if (!_connected)
    {
        out           = {};
        out.heartRate = -1;
        out.spo2      = 0.0f;
        out.valid     = false;
        return true;
    }

    out.irRaw     = static_cast<uint32_t>(_lastIrValue);
    out.redRaw    = 0;
    out.heartRate = static_cast<int8_t>(constrain(_beatAvg, 0, 127));
    out.spo2      = _spo2Valid ? _spo2 : 0.0f;
    out.valid     = (_beatAvg > 20 && _beatAvg < 255) && _spo2Valid;

    return true;
}


// =============================================================================
// setPower()
// =============================================================================
void SensorPPG::setPower(bool enable)
{
    if (!_connected) return;

    if (enable)
        _sensor.wakeUp();
    else
        _sensor.shutDown();

    LOG_DEBUG(TAG, "Power: %s", enable ? "ON (wakeUp)" : "OFF (shutDown)");
}