// File: firmware/lib/HealthSensors/Sensor_PPG.cpp

// =============================================================================
// Sensor_PPG.cpp — Implementasi Driver MAX30102
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "Sensor_PPG.h"
#include "../../include/Config.h"
#include <Wire.h>
#include "heartRate.h" // algoritma beat detection dari library SparkFun

static constexpr char TAG[] = "PPG";


// =============================================================================
// begin() — Inisialisasi Sensor
//
// Wire.begin() dipanggil di sini — bukan di main.cpp — karena inisialisasi
// bus I2C adalah tanggung jawab driver, bukan orkestrator.
// Lihat catatan urutan inisialisasi di header file.
// =============================================================================
bool SensorPPG::begin()
{
    Wire.begin(Pin::PPG_SDA, Pin::PPG_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(100);

    // Retry hingga 3x — MAX30102 kadang butuh waktu saat power-on
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
    return true;
}


// =============================================================================
// update() — Update State Heart Rate
//
// Algoritma beat detection dari library SparkFun (checkForBeat):
//   1. Baca nilai IR terbaru dari sensor
//   2. Jika terdeteksi beat, hitung BPM dari delta waktu antar beat
//   3. Simpan ke circular buffer _rates[RATE_SIZE] lalu rata-ratakan
//
// Panggil fungsi ini sesering mungkin (idealnya tanpa delay di task loop)
// agar tidak ada beat yang terlewat — terutama saat HR tinggi (>100 BPM).
// =============================================================================
void SensorPPG::update()
{
    if (!_connected) return;

    const long irValue = _sensor.getIR();
    if (irValue == 0) return;

    _lastIrValue = irValue;

    if (!checkForBeat(irValue)) return;

    // Beat terdeteksi — hitung BPM dari interval antar beat
    const long delta    = millis() - _lastBeatMs;
    _lastBeatMs         = millis();
    _beatsPerMinute     = 60.0f / (delta / 1000.0f);

    // Hanya simpan BPM yang masuk akal secara fisiologis (20–254 BPM)
    if (_beatsPerMinute < 255 && _beatsPerMinute > 20)
    {
        _rates[_rateSpot++] = static_cast<byte>(_beatsPerMinute);
        _rateSpot %= RATE_SIZE;

        // Rata-ratakan semua entry di circular buffer
        _beatAvg = 0;
        for (byte i = 0; i < RATE_SIZE; i++)
            _beatAvg += _rates[i];
        _beatAvg /= RATE_SIZE;

        LOG_DEBUG(TAG, "Beat! BPM=%.1f | avg=%d BPM | IR=%ld",
                  _beatsPerMinute, _beatAvg, irValue);
    }
}


// =============================================================================
// read() — Salin State Terkini ke PpgSample
//
// Selalu mengisi `out` bahkan saat sensor tidak terhubung
// (isi dengan nol/nilai invalid) agar caller tidak perlu cek isConnected()
// sebelum memanggil read().
// =============================================================================
bool SensorPPG::read(PpgSample& out)
{
    if (!_connected)
    {
        out = {}; // zero-initialize semua field
        out.heartRate = -1;
        out.valid      = false;
        return true;
    }

    out.irRaw     = static_cast<uint32_t>(_lastIrValue);
    out.redRaw    = 0; // tidak dipakai saat ini
    out.heartRate = static_cast<int8_t>(constrain(_beatAvg, 0, 127));
    out.spo2       = 0.0f; // kalkulasi SpO2 belum diimplementasikan
    out.valid      = (_beatAvg > 20 && _beatAvg < 255);

    return true;
}


// =============================================================================
// setPower() — Toggle Power Mode Sensor
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