// File: firmware/lib/HealthSensors/Sensor_PPG_Finger.cpp

#ifdef USE_PPG_FINGER

// =============================================================================
// Sensor_PPG_Finger.cpp — Driver MAX30102 (FINGER). Implementasi tipis.
// =============================================================================
//
// Tanggung jawab file ini hanya:
//   1. Inisialisasi & konfigurasi hardware MAX30102.
//   2. Membaca sampel IR/Red tiap update().
//   3. Mendeteksi kontak kulit (jari/pergelangan menempel).
//   4. Meneruskan sampel ke HeartRateMonitor (semua DSP detak ada di sana).
//   5. Menghitung SpO2 via Ratio-of-Ratios saat detak terdeteksi.
//
// Semua log via makro LOG_* dari utils/Logger.h.
// =============================================================================

#include "Sensor_PPG.h"
#include "../../include/Config.h"
#include <Wire.h>

static constexpr char TAG[] = "PPG";


// =============================================================================
// Lookup Table SpO2 (R -> SpO2%). Index = R*10, valid R in [0.4, 3.4]
// Diturunkan dari fit empiris Maxim AN6945.
// =============================================================================
static const uint8_t SPO2_TABLE[35] = {
    100, 100, 100, 100, 100,  99,  98,  97,
     96,  95,  94,  93,  92,  91,  90,  89,
     87,  86,  84,  82,  81,  79,  77,  75,
     73,  71,  69,  67,  65,  63,  61,  60,
     60,  60,  60
};
static constexpr uint8_t SPO2_TABLE_SIZE = sizeof(SPO2_TABLE);


// =============================================================================
// begin() — Inisialisasi & konfigurasi hardware
// =============================================================================
bool SensorPPG::begin()
{
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(10);

    for (uint8_t attempt = 1; attempt <= 3; attempt++)
    {
        if (_sensor.begin(Wire, I2C_SPEED_FAST))
            break;

        if (attempt == 3)
        {
            LOG_ERROR(TAG, "MAX30102 tidak ditemukan setelah 3x retry | SDA=%d SCL=%d",
                      Pin::I2C_SDA, Pin::I2C_SCL);
            _connected = false;
            return false;
        }
        LOG_WARN(TAG, "Retry %d/3 ...", attempt);
        delay(50);
    }

    // Konfigurasi eksplisit (parameter benar-benar dipakai, bukan default).
    _sensor.setup(PpgConfig::LED_POWER_FINGER, PpgConfig::SAMPLE_AVG, PpgConfig::LED_MODE,
                  PpgConfig::SAMPLE_RATE, PpgConfig::PULSE_WIDTH, PpgConfig::ADC_RANGE);
    _sensor.setPulseAmplitudeRed(PpgConfig::LED_POWER_FINGER);
    _sensor.setPulseAmplitudeIR(PpgConfig::LED_POWER_FINGER);

    _hr.reset();
    _connected = true;

    LOG_INFO(TAG, "MAX30102 siap (FINGER) | SDA=%d SCL=%d | LED=0x%02X",
             Pin::I2C_SDA, Pin::I2C_SCL, PpgConfig::LED_POWER_FINGER);
    LOG_INFO(TAG, "Pipeline: bandpass -> motion-gate -> envelope -> beat -> bpm");
    return true;
}


// =============================================================================
// update() — Baca sampel, kelola kontak, jalankan pipeline
// =============================================================================
void SensorPPG::update()
{
    if (!_connected) return;

    const long ir  = _sensor.getIR();
    const long red = _sensor.getRed();
    if (ir == 0) return;

    _lastIr  = ir;
    _lastRed = red;

    const uint32_t now = millis();
    const bool contact = (static_cast<uint32_t>(ir) > IR_CONTACT_MIN);

    // ── Kontak hilang -> reset pipeline & SpO2 ────────────────────────────────
    if (!contact)
    {
        if (_contact)
        {
            LOG_DEBUG(TAG, "Kontak hilang (IR=%ld)", ir);
            _hr.reset();
            _spo2Valid = false;
            _spo2 = 0.0f;
            _bufFill = 0; _bufHead = 0; _rBufFill = 0;
        }
        _contact = false;
        return;
    }

    // ── Kontak baru -> seed filter (hindari transient) ────────────────────────
    if (!_contact)
    {
        _contact = true;
        _hr.onContact(static_cast<float>(ir), now);
        LOG_DEBUG(TAG, "Kontak terdeteksi (IR=%ld)", ir);
    }

    // ── Isi ring buffer SpO2 ──────────────────────────────────────────────────
    _irBuf[_bufHead]  = static_cast<int32_t>(ir);
    _redBuf[_bufHead] = static_cast<int32_t>(red);
    _bufHead = (_bufHead + 1) % SPO2_FIFO_SIZE;
    if (_bufFill < SPO2_FIFO_SIZE) _bufFill++;

    // ── Jalankan pipeline detak jantung ───────────────────────────────────────
    const bool beat = _hr.update(static_cast<float>(ir), now);

    if (beat)
    {
        LOG_DEBUG(TAG, "Beat! BPM=%d | bpf=%.1f thr=%.1f",
                  _hr.getBpm(), _hr.getFilteredSignal(), _hr.getThreshold());

        // ── Hitung SpO2 saat detak terdeteksi & buffer penuh ──────────────────
        if (_bufFill >= SPO2_FIFO_SIZE)
            _updateSpo2(ir, red, true);
    }
}


// =============================================================================
// _updateSpo2() — Ratio-of-Ratios dari ring buffer
// =============================================================================
void SensorPPG::_updateSpo2(long /*ir*/, long /*red*/, bool /*beatJustDetected*/)
{
    // Susun ring buffer ke array linear kronologis.
    int32_t irLin[SPO2_FIFO_SIZE];
    int32_t redLin[SPO2_FIFO_SIZE];
    for (uint8_t i = 0; i < SPO2_FIFO_SIZE; i++)
    {
        uint8_t idx = (_bufHead + i) % SPO2_FIFO_SIZE;
        irLin[i]  = _irBuf[idx];
        redLin[i] = _redBuf[idx];
    }

    float acIr = 0, dcIr = 0, acRed = 0, dcRed = 0;
    if (!_acdc(irLin, SPO2_FIFO_SIZE, acIr, dcIr) ||
        !_acdc(redLin, SPO2_FIFO_SIZE, acRed, dcRed))
    {
        _spo2Valid = false;
        return;
    }

    if (dcIr < 1.0f || dcRed < 1.0f || acIr < 1.0f)
    {
        _spo2Valid = false;
        return;
    }

    const float R = (acRed / dcRed) / (acIr / dcIr);

    _rBuf[_rBufSpot++] = R;
    _rBufSpot %= SPO2_BEAT_AVG;
    if (_rBufFill < SPO2_BEAT_AVG) _rBufFill++;

    if (_rBufFill < SPO2_MIN_BEATS)
    {
        _spo2Valid = false;
        return;
    }

    float rAvg = 0;
    for (uint8_t i = 0; i < _rBufFill; i++) rAvg += _rBuf[i];
    rAvg /= _rBufFill;

    const float spo2 = _rToSpo2(rAvg);
    if (spo2 >= SPO2_VALID_MIN && spo2 <= SPO2_VALID_MAX)
    {
        _spo2 = spo2;
        _spo2Valid = true;
        LOG_DEBUG(TAG, "SpO2=%.1f%% | R=%.3f", _spo2, rAvg);
    }
    else
    {
        _spo2Valid = false;
        LOG_WARN(TAG, "SpO2 out of range: %.1f%% (R=%.3f)", spo2, rAvg);
    }
}


// =============================================================================
// _acdc() — AC (peak-to-peak) & DC (mean)
// =============================================================================
bool SensorPPG::_acdc(const int32_t* buf, uint8_t len,
                      float& out_ac, float& out_dc)
{
    if (len == 0) return false;

    int32_t vmin = buf[0], vmax = buf[0];
    int64_t sum = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        if (buf[i] < vmin) vmin = buf[i];
        if (buf[i] > vmax) vmax = buf[i];
        sum += buf[i];
    }
    out_ac = static_cast<float>(vmax - vmin);
    out_dc = static_cast<float>(sum) / static_cast<float>(len);
    return (out_ac > 0.0f && out_dc > 0.0f);
}


// =============================================================================
// _rToSpo2() — Lookup R -> SpO2 dengan interpolasi linear
// =============================================================================
float SensorPPG::_rToSpo2(float R)
{
    if (R < 0.0f) R = 0.0f;

    const float idxF = R * 10.0f;
    const int   idx0 = static_cast<int>(idxF);
    const int   idx1 = idx0 + 1;
    const float frac = idxF - static_cast<float>(idx0);

    const int i0 = (idx0 < 0) ? 0
                 : (idx0 >= SPO2_TABLE_SIZE) ? (SPO2_TABLE_SIZE - 1) : idx0;
    const int i1 = (idx1 >= SPO2_TABLE_SIZE) ? (SPO2_TABLE_SIZE - 1) : idx1;

    return static_cast<float>(SPO2_TABLE[i0]) * (1.0f - frac)
         + static_cast<float>(SPO2_TABLE[i1]) * frac;
}


// =============================================================================
// read() — Salin state ke PpgMeasurement
// =============================================================================
bool SensorPPG::read(PpgMeasurement& out)
{
    if (!_connected)
    {
        out           = {};
        out.heartRate = -1;
        out.spo2      = 0.0f;
        out.valid     = false;
        return true;
    }

    out.irRaw     = static_cast<uint32_t>(_lastIr);
    out.redRaw    = static_cast<uint32_t>(_lastRed);
    out.heartRate = static_cast<int8_t>(constrain(_hr.getBpm(), 0, 127));
    out.spo2      = _spo2Valid ? _spo2 : 0.0f;
    out.valid     = _hr.isValid() && _spo2Valid;
    return true;
}


// =============================================================================
// setPower()
// =============================================================================
void SensorPPG::setPower(bool enable)
{
    if (!_connected) return;
    if (enable) _sensor.wakeUp();
    else        _sensor.shutDown();
    LOG_DEBUG(TAG, "Power: %s", enable ? "ON" : "OFF");
}

#endif // USE_PPG_FINGER