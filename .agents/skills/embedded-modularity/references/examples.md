# Contoh Before/After — Embedded Modularity

Referensi ini dipanggil dari `SKILL.md` saat butuh contoh kode konkret. Tiga skenario paling umum yang bikin firmware "nyampur".

---

## 1. Driver yang diam-diam import layer network

**Sebelum** (driver tahu soal mesh — salah arah dependency):

```cpp
// Sensor_PPG.h
#include "MeshPackets.h"   // <-- driver TIDAK PERLU tahu format mesh

class SensorPPG
{
public:
    bool read(PpgSample& out);   // PpgSample didefinisikan di MeshPackets.h
};
```

**Sesudah** (driver punya struct sendiri, network layer yang mengonversi):

```cpp
// Sensor_PPG.h — TIDAK ada #include "MeshPackets.h"
struct PpgSample
{
    uint32_t irRaw = 0, redRaw = 0;
    int8_t   heartRate = -1;
    float    spo2 = 0.0f;
    bool     valid = false;
};

class SensorPPG
{
public:
    bool begin();
    void update();
    bool read(PpgSample& out);
};
```

```cpp
// Orchestrator.cpp (atau main.cpp) — layer yang memang tahu soal mesh
#include "Sensor_PPG.h"
#include "MeshPackets.h"

void sendHealthData(SensorPPG& ppg, MeshSender& mesh)
{
    PpgSample s;
    ppg.read(s);

    MeshPacket pkt;
    pkt.heartRate = s.heartRate;
    pkt.spo2      = s.spo2;
    mesh.send(pkt);
}
```

Kenapa ini penting: `Sensor_PPG.h` sekarang bisa dipakai ulang di project lain yang tidak punya mesh sama sekali, dan bisa di-compile/test tanpa `MeshPackets.h` ikut berubah setiap kali protokol mesh di-update.

---

## 4. Komentar yang profesional vs yang mengotori kode

**Sebelum** — komentar hanya menerjemahkan kode, tidak menambah informasi:

```cpp
// =============================================================================
// Sensor_MPU.h — Driver MPU6050
// =============================================================================
// Bus: Wire1, pin SDA=21, SCL=22   // <-- salah, tidak diupdate setelah refactor
// =============================================================================

// Baca data sensor
bool read(ImuSample& out);

// Set sleep
void setSleep(bool enable);
```

```cpp
// begin() — inisialisasi sensor
bool SensorMPU::begin()
{
    Wire.begin(21, 22);   // mulai Wire
    delay(100);           // delay 100ms
    Wire.write(0x00);     // tulis 0x00
    Wire.write(0x04);     // tulis 0x04
}
```

Masalah: komentar di header sudah stale (menyebut Wire1 padahal kode pakai Wire), komentar di `.cpp` cuma echo kode tanpa menjelaskan keputusan teknis apapun.

**Sesudah** — komentar menjawab pertanyaan yang tidak bisa dijawab kode sendiri:

```cpp
// =============================================================================
// Sensor_MPU.h — Driver MPU6050 (Accelerometer + Gyroscope)
// =============================================================================
//
// Hardware  : MPU6050 (termasuk varian KW/clone)
//             Bus Wire, pin dikonfigurasi lewat Pin::I2C_SDA / Pin::I2C_SCL di Config.h
//
// Kenapa implementasi manual (tanpa library)?
//   Library MPU6050 umum tidak kompatibel dengan sensor kloningan.
//   Implementasi raw I2C register lebih robust untuk hardware KW.
//
// CARA PAKAI:
//   SensorMPU imu;
//   imu.begin();           // inisialisasi & verifikasi koneksi
//   imu.calibrate();       // opsional, letakkan sensor datar & diam
//
//   ImuSample data;
//   if (imu.read(data)) { /* gunakan data.accelX, data.gyroY, dst. */ }
//
// THREAD SAFETY:
//   Tidak thread-safe. Gunakan mutex di luar modul ini jika diakses dari beberapa task.
// =============================================================================

// Inisialisasi Wire, wake sensor dari sleep, config DLPF, dan verifikasi burst read.
// Return false jika sensor tidak merespons atau burst read gagal (<14 byte).
// Otomatis load kalibrasi NVS jika ada — tidak perlu panggil loadCalibration() manual.
bool begin();

// Toggle sleep mode. enable=true → konsumsi turun ke ~5µA (SLEEP bit register 0x6B).
void setSleep(bool enable);
```

```cpp
bool SensorMPU::begin()
{
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(10); // 10ms cukup — tidak pakai kabel panjang yang butuh stabilisasi lebih lama

    // Wake dari sleep mode: clear bit 6 (SLEEP) di PWR_MGMT_1.
    // Sensor default sleep setelah power-on sesuai datasheet §4.28.
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1);
    Wire.write(0x00);
    const uint8_t err = Wire.endTransmission();
    ...

    // DLPF_CFG = 4 → bandwidth ~21 Hz.
    // Krusial untuk anti-aliasing sebelum sampling 50 Hz (Nyquist = 25 Hz)
    // tanpa beban komputasi tambahan di ESP32.
    Wire.write(0x04);
    ...
}
```

Perbedaan kunci:
- Header mendeskripsikan **kontrak** (cara pakai, thread safety, alasan desain) — bukan daftar method
- Komentar register menjelaskan **efek dan alasan teknis** angka tersebut dipilih
- Pin tidak di-hardcode di komentar — dirujuk ke `Config.h` supaya tidak stale saat refactor
- `delay(10)` dikomentar *kenapa* 10ms (bukan 100ms seperti biasanya)
 (anti-pattern, jangan ditiru)

**Sebelum** — kalkulasi FFT/BPM langsung di kelas driver, tidak bisa dites tanpa hardware:

```cpp
class PPGHandler {
public:
    bool begin();
    void update(bool isMoving);
private:
    PPGData processBatch();   // FFT, averaging, semua logic DSP di sini
    MAX30105 sensor;          // <-- hardware
    ArduinoFFT<float> FFT;    // <-- algoritma, nyampur sama hardware di kelas yang sama
    float vReal[BUFFER_SIZE], vImag[BUFFER_SIZE];
};
```

Masalah: untuk test logic FFT-nya saja, Anda terpaksa ikut bawa `MAX30105` dan seluruh state hardware walau cuma mau verifikasi matematika.

**Sesudah** — pisah seperti pola `HeartRateMonitor` + `PpgDsp.h`:

```cpp
// PpgDsp.h — header-only, no hardware, testable berdiri sendiri
namespace ppgdsp {
class BandPass { /* process(x) -> y, reset() */ };
class PeakEnvelope { /* ... */ };
class BeatDetector { /* ... */ };
}

// HeartRateMonitor.h — merangkai komponen DSP, masih no-hardware
class HeartRateMonitor
{
public:
    bool update(float irValue, uint32_t nowMs);  // sampel masuk, tidak tahu soal I2C
private:
    ppgdsp::BandPass     _bandpass;
    ppgdsp::PeakEnvelope _envelope;
    ppgdsp::BeatDetector _detector;
};

// Sensor_PPG.h/.cpp — driver TIPIS, cuma baca register & teruskan ke pipeline
class SensorPPG
{
    void update() {
        long ir = _sensor.getIR();          // satu-satunya yang sentuh hardware
        _hr.update(static_cast<float>(ir), millis());  // serahkan ke layer algorithm
    }
private:
    MAX30105 _sensor;
    HeartRateMonitor _hr;
};
```

Sekarang `HeartRateMonitor` bisa di-unit-test dengan array sampel IR palsu, tanpa MAX30102 fisik.

---

## 3. Orchestrator yang bersih vs yang nyampur

**Sebelum** — `loop()` berisi logika domain (kalkulasi rata-rata, kondisi tidur, dsb):

```cpp
void loop() {
    long ir = sensor.getIR();
    static float sumBpm = 0;
    static int count = 0;
    // ... kalkulasi rata-rata BPM manual di sini ...
    if (count >= 5) {
        float avgBpm = sumBpm / count;
        // ... logika valid/tidak valid ...
    }
}
```

**Sesudah** — `loop()` cuma orkestrasi, semua kalkulasi sudah dibungkus method:

```cpp
void loop() {
    bodySensor.update();
    airSensor.update();
    ppgSensor.update();

    if (ppgSensor.hasNewData()) {
        bleServer.sendHeartData(ppgSensor.getBPM(), ppgSensor.getSpO2());
        ppgSensor.clearNewData();
    }
}
```

Pola `begin()` semua di `setup()`, `update()` semua di awal `loop()`, lalu blok `if` pendek untuk cek-dan-kirim — itulah yang membuat orchestrator gampang dipindai walau jumlah sensornya bertambah banyak.