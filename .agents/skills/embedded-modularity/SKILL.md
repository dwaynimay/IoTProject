---
name: embedded-modularity
description: Use this skill whenever writing, reviewing, or refactoring embedded C++ firmware (Arduino/ESP32/STM32/etc) — driver classes, sensor handlers, DSP/signal-processing pipelines, BLE/mesh/network layers, or any main.cpp/setup()/loop() orchestration. Trigger this for ANY firmware code task even if the user doesn't explicitly say "modular" or "clean code" — e.g. "tambah driver sensor baru", "kenapa kode saya nyampur", "buatkan kelas untuk X", "refactor file ini", "kok hardware code-nya ketuker sama logic", or simply pasting a .h/.cpp file and asking for changes. Enforces strict layering (hardware driver vs algorithm/DSP vs orchestration/network), a uniform begin()/update()/getX() public API, centralized config, and zero circular or upward dependencies — so firmware reads like a clean reusable library, not a tangled sketch.
---

# Embedded Firmware Modularity

Tujuan skill ini: setiap kali Claude menulis atau merapikan kode firmware embedded C++, hasilnya terasa seperti **library yang tinggal panggil-panggil** — bukan satu sketch besar yang semua urusannya nyampur. Ini bukan soal estetika; arsitektur yang benar membuat tiap layer **bisa diuji tanpa hardware**, **bisa dipakai ulang di project lain**, dan **gampang dipindai** orang lain (atau Claude sendiri, sesi berikutnya).

Baca seluruh file ini sebelum menulis atau mengedit kode firmware apa pun. Untuk contoh kode lengkap before/after, baca `references/examples.md`.

## Mental model: 4 layer, satu arah panah

Setiap firmware project, sekecil apa pun, dipecah ke 4 layer ini. Panah dependency **hanya boleh mengalir ke bawah** — layer atas boleh `#include` layer bawah, tidak pernah sebaliknya.

```
1. main.cpp / Orchestrator   (setup(), loop(), pemilihan kapan kirim/kapan baca)
        |
2. Network/Protocol layer    (BLE_Handler, MeshPackets, Wifi — "bagaimana data dikirim keluar")
        |
3. Domain/Algorithm layer    (HeartRateMonitor, Spo2Monitor, FilterChain — DSP murni, no hardware)
        |
4. Hardware Driver layer     (Sensor_PPG, Sensor_IMU — I2C/SPI/register access only)
```

**Aturan keras**: layer 4 (driver) **tidak boleh** tahu apa-apa soal layer 2 (network/mesh/BLE). Driver tidak `#include "MeshPackets.h"`, tidak `#include "BLE_Handler.h"`. Driver hanya tahu cara bicara ke chip dan menyerahkan data lewat struct miliknya sendiri (lihat "Tipe data milik layer-nya sendiri" di bawah).

Kalau menemukan driver yang `#include` sesuatu dari layer 2 atau 1, itu **bug arsitektur** — selalu tunjukkan ke user dan tawarkan perbaikan, walau user tidak memintanya secara eksplisit.

### Cara cepat klasifikasi sebuah file

Tanyakan: *"Kalau saya cabut chip ini dari board dan ganti simulator, apakah file ini masih kompilasi tanpa ubah satu baris pun?"*
- Ya → ini layer 3 (algorithm/DSP), HARUS bisa diuji tanpa hardware.
- Tidak, tapi alasannya cuma "panggil `Wire.h`/register/I2C" → layer 4 (driver), itu wajar, tugasnya memang bicara ke hardware.
- Tidak, dan alasannya "perlu tahu format BLE/mesh packet" → ini layer 2, jangan dicampur ke driver atau algorithm.

## API publik yang seragam

Setiap kelas driver atau handler — apa pun sensornya — punya bentuk permukaan yang sama persis, supaya orchestrator (`main.cpp`) bisa "tinggal panggil-panggil" tanpa perlu ingat API unik per sensor:

```cpp
class SensorX
{
public:
    bool begin();              // init hardware, return false kalau gagal — JANGAN throw/hang
    void update();              // dipanggil tiap loop(), non-blocking
    bool read(XSample& out);    // salin state terakhir ke struct output

    // getter monitoring/debug, semua const, semua nama getX()
    float getY() const { return _y; }
    bool  isValid() const { return _valid; }

private:
    // implementasi
};
```

Konsekuensi dari kontrak ini:
- `begin()` **selalu** mengembalikan `bool`, tidak pernah `void`. Caller (orchestrator) wajib bisa cek kegagalan tanpa baca log.
- `update()` **tidak boleh blocking** (tidak ada `delay()` panjang di dalamnya) — ia dipanggil tiap iterasi `loop()`.
- Penamaan getter konsisten: `getX()` untuk nilai, `isX()`/`hasX()` untuk boolean — bukan campur `getBpm()` dengan `Spo2()` dengan `fetchTemp()`.
- Kalau ada N sensor berjenis sama (mis. semua "Handler"), pertahankan nama method yang sama persis di semua kelasnya. Lihat pola `begin()/update()/getX()` yang dipakai konsisten lintas `BME680_Handler`, `MAX30205_Handler`, `BMI270_Handler`, dst — itulah yang bikin `main.cpp` enak dibaca tanpa harus loncat-loncat baca header.

## Tipe data milik layer-nya sendiri

Setiap layer mendefinisikan struct datanya **sendiri**, bukan meminjam struct dari layer di atasnya:

```cpp
// Sensor_PPG.h (layer 4 — driver)
struct PpgSample {
    uint32_t irRaw = 0, redRaw = 0;
    int8_t   heartRate = -1;
    float    spo2 = 0.0f;
    bool     valid = false;
};
```

Layer network (BLE/mesh) yang **mengonversi** dari `PpgSample` ke format paketnya sendiri (`MeshPacket`), bukan driver yang dipaksa tahu bentuk packet:

```cpp
// di Orchestrator/Network layer, BUKAN di driver
PpgSample s; ppg.read(s);
MeshPacket pkt;
pkt.heartRate = s.heartRate;
pkt.spo2      = s.spo2;
mesh.send(pkt);
```

Kalau struct outputnya cuma dipakai 1 tempat dan sangat kecil (2-3 field primitif), boleh return langsung tanpa struct — tapi begitu lebih dari ~4 field atau dipakai lintas file, jadikan named struct di layer asalnya.

## Algorithm/DSP layer harus testable tanpa hardware

Pola terbaik (sudah Anda terapkan di `HeartRateMonitor` + `PpgDsp.h`): pisahkan komponen sinyal jadi kelas-kelas kecil header-only, masing-masing satu tanggung jawab, dengan `reset()` dan `process(x) -> y`. Pipeline merangkainya. **Pertahankan pola ini** — jangan pernah menulis ulang DSP langsung di dalam driver hanya karena "lebih cepat".

Ciri layer ini benar:
- Tidak ada `#include <Wire.h>`, tidak ada `MAX30105.h`, tidak ada apa pun yang menyentuh bus hardware.
- Constructor menerima parameter tuning sebagai angka (bukan baca dari `Config.h` langsung di tubuh kelas) — supaya bisa diuji dengan parameter berbeda-beda di unit test.
- Semua state lewat member, bukan `static`/global.

## Config tersentralisasi

Semua pin, alamat I2C, UUID BLE, dan konstanta kalibrasi tinggal di satu `Config.h` (atau namespace `Pin::`, `I2CClock::` seperti yang sudah Anda pakai) — bukan tersebar sebagai magic number di tiap driver. Driver menerima nilai itu lewat `Config::` namespace atau parameter constructor, bukan hardcode angka literal di tengah method.

Saat menulis driver baru atau mengedit driver lama, kalau menemukan angka literal yang berarti khusus (pin number, threshold ADC, alamat I2C, UUID) langsung di badan kode — pindahkan ke `Config.h` dan beri nama, lalu referensikan dari sana.

## Orchestrator (`main.cpp`) harus bodoh

`setup()`/`loop()` **tidak boleh** berisi logika DSP, parsing, atau kalkulasi. Isinya cuma:
1. Deklarasi instance tiap handler/driver.
2. Panggil `.begin()` semua di `setup()`.
3. Panggil `.update()` semua di `loop()`.
4. Cek `.hasNewData()`/`.isValid()` lalu teruskan ke layer network.

Kalau menemukan `if`/`for`/kalkulasi matematis lebih dari sekadar perbandingan timestamp (`now - last > interval`) di dalam `loop()`, itu tanda logika yang seharusnya pindah ke layer algorithm atau network.

## Komentar yang profesional

Komentar bukan dekorasi dan bukan terjemahan kode ke bahasa manusia. Satu-satunya alasan menulis komentar adalah untuk menjawab pertanyaan yang **tidak bisa dijawab oleh kode itu sendiri**: kenapa keputusan ini diambil, apa trade-off-nya, apa asumsi yang harus benar supaya kode ini benar.

### Prinsip utama: jelaskan *kenapa*, bukan *apa*

```cpp
// BURUK — hanya menerjemahkan kode, tidak menambah informasi:
Wire.write(0x04); // tulis 0x04 ke register CONFIG

// BAIK — menjelaskan keputusan teknis dan konsekuensinya:
// DLPF_CFG = 4 → bandwidth ~21 Hz. Krusial untuk anti-aliasing sebelum
// sampling 50 Hz (Nyquist = 25 Hz) tanpa beban komputasi di MCU.
Wire.write(0x04);
```

Kalau kode sudah jelas dibaca sendiri (`Wire.write(0x00); // wake up`), satu komentar pendek di ujung baris cukup. Komentar panjang hanya untuk keputusan yang tidak obvious — magic number register, workaround hardware, atau rumus yang punya alasan fisik/matematis tertentu.

### File header (.h): kontrak, bukan tutorial

Komentar di `.h` adalah kontrak publik kelas — apa yang dijanjikan, apa prasyaratnya, apa yang tidak ditangani. Format yang konsisten:

```cpp
// =============================================================================
// NamaKelas — Satu kalimat: apa yang dilakukan kelas ini
// =============================================================================
//
// Hardware  : chip apa, bus apa, pin berapa
// Kenapa implementasi ini: alasan teknis kalau ada pilihan non-obvious
//             (mis: "implementasi manual karena library umum tidak kompatibel sensor KW")
//
// CARA PAKAI:
//   NamaKelas obj;
//   obj.begin();
//   obj.update();   // panggil tiap loop()
//   auto v = obj.getValue();
//
// THREAD SAFETY:
//   Sebutkan secara eksplisit — "tidak thread-safe", atau "aman dari ISR untuk read".
//   Jangan biarkan kosong; ketiadaan info = caller tidak tahu harus pakai mutex atau tidak.
// =============================================================================
```

Untuk method publik di `.h`, komentar **di atas deklarasi** (bukan di baris yang sama) kalau penjelasannya lebih dari ~6 kata:

```cpp
// Inisialisasi bus I2C, wake sensor dari sleep, dan verifikasi burst read.
// Return false jika sensor tidak merespons — caller wajib cek return value ini.
bool begin();

bool isConnected() const { return _connected; } // tidak perlu komentar — nama sudah cukup
```

### Section divider di .cpp: konsisten atau tidak sama sekali

Kalau file `.cpp` panjang dan pakai section divider (`// === Nama ===`), **semua** section harus pakai gaya yang sama persis. Jangan campur `//---`, `// ***`, dan `// ===` dalam satu file. Gaya yang dipakai di codebase ini:

```cpp
// =============================================================================
// NamaMethod() — Satu kalimat ringkas tujuan method
// =============================================================================
// Paragraf opsional kalau ada konteks penting: asumsi input, edge case,
// atau alasan implementasi yang tidak obvious dari nama method saja.
// =============================================================================
```

### Sinkronisasi komentar dengan kode (wajib dicek setiap edit)

Komentar yang salah lebih berbahaya dari tidak ada komentar — pembaca percaya komentar, lalu debugging ke arah yang salah.

**Aturan**: setiap kali mengedit kode, scan komentar di sekitarnya. Kalau ada yang tidak lagi akurat, update sekarang juga — bukan nanti. Ini termasuk:

- Komentar header `.h` yang menyebut pin, bus, atau library yang sudah berubah
- Komentar register/magic number yang nilainya berubah
- Komentar "cara pakai" yang urutannya sudah tidak relevan
- Deskripsi hardware di file header (`// pin SDA=21, SCL=22`) yang sudah berpindah

**Contoh kasus nyata** (dari codebase ini): `Sensor_MPU.h` menyebut `Wire1` dan `pin SDA=21 SCL=22`, padahal implementasi di `.cpp` pakai `Wire` biasa dengan pin dari `Config.h`. Ini harus dikoreksi bersamaan dengan perubahan kode yang menyebabkannya — bukan dibiarkan karena "bukan fokus PR/task ini".

Setiap kali menemukan inkonsistensi komentar-vs-kode seperti ini saat review atau edit, **selalu laporkan ke user secara spesifik** (file mana, baris berapa, apa yang salah) walau user tidak memintanya — komentar salah adalah bug laten.

### Yang tidak perlu dikomentar

- Nama method yang sudah self-explanatory (`isConnected()`, `clearCalibration()`)
- Getter/setter trivial satu baris
- `#include` — tidak perlu komentar kenapa di-include kecuali dependency-nya tidak obvious
- Loop/kondisi standar yang sudah jelas dari nama variabelnya

### Bahasa komentar

Ikuti bahasa yang sudah dipakai di codebase. Kalau codebase campur (Indonesia + Inggris untuk istilah teknis), pertahankan pola itu — jangan tiba-tiba ganti semua ke English atau semua ke Indonesia saat mengedit sebagian file.

## Checklist sebelum menyerahkan kode ke user

Setiap kali selesai menulis/mengedit file firmware, jalankan checklist ini secara diam-diam di kepala sebelum present ke user — kalau ada yang gagal, perbaiki dulu atau jelaskan ke user kenapa belum:

1. Driver (`#include` MAX30105/Wire/dsb) tidak `#include` apa pun dari layer network/mesh/BLE.
2. Semua kelas punya `begin() -> bool`, `update() -> void`, getter konsisten `getX()`/`isX()`.
3. Tidak ada angka literal "berarti" (pin, address, threshold kalibrasi) yang bukan dari `Config.h`.
4. Komponen algorithm/DSP bisa dibayangkan dicompile berdiri sendiri tanpa hardware header.
5. `main.cpp`/orchestrator tidak mengandung kalkulasi domain — cuma pemanggilan method.
6. Struct data dimiliki oleh layer yang membuatnya, dikonversi (bukan dipinjam) saat naik ke layer atas.

7. Komentar menjelaskan *kenapa*, bukan *apa* — tidak ada komentar yang cuma menerjemahkan kode.
8. Komentar header `.h` akurat: nama bus, pin, dan cara pakai sesuai implementasi aktual di `.cpp`.
9. Kalau ada section divider di `.cpp`, formatnya konsisten di seluruh file.
10. Komentar yang sudah tidak akurat akibat edit ini sudah diupdate — tidak ada komentar berbohong yang tertinggal.

Jika user memberi kode yang melanggar salah satu poin di atas, **selalu beri tahu secara spesifik poin mana yang dilanggar dan kenapa**, baru tawarkan perbaikan — jangan diam-diam dibiarkan kalau itu bukan fokus permintaan user, cukup sebutkan singkat lalu fokus ke permintaan utama.

## Lihat juga

- `references/examples.md` — contoh before/after lengkap: driver yang ketahuan import mesh packet, DSP yang nyampur ke driver, dan orchestrator yang bersih ala `main.cpp`.