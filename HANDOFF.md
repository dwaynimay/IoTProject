# Handoff — Refactor ESP32 Health Monitor Mesh

> Dokumen ini dibuat untuk memudahkan perpindahan sesi.
> Update bagian **Status** setiap kali satu item selesai.

---

## Ringkasan Project

**Apa ini?**
Sistem monitoring kesehatan berbasis 3 ESP32 (mesh):
- **Node A & B** — sensor (MPU6050 IMU + MAX30102 PPG), encode sinyal via Compressive Sensing, kirim ke gateway via ESP-NOW
- **Node C (Gateway)** — terima dari A & B, forward ke MQTT broker via WiFi
- **Server Python** — subscribe MQTT, rekonstruksi sinyal CS via LASSO, visualisasi real-time

**Tech stack:**
- Firmware: C++ / Arduino / FreeRTOS / PlatformIO
- Transport: ESP-NOW (sensor↔gateway) + MQTT (gateway↔server)
- Server: Python 3, paho-mqtt, numpy, scikit-learn, scipy, matplotlib

---

## Peta File Project

```
project/
├── platformio.ini                  # tidak berubah
├── .gitignore                      # ✅
│
├── include/
│   ├── Config.h                    # ✅ forward-include saja
│   ├── config/
│   │   ├── credentials.h           # ✅
│   │   ├── credentials.h.example   # ✅
│   │   ├── hardware.h              # ✅
│   │   ├── features.h              # ✅
│   │   └── tuning.h                # ✅
│   ├── CS_Sensor.h                 # ✅ CSPhiMatrix singleton + CSEncoder
│   ├── DataModels.h                # ✅ + RawPacket struct (item #5)
│   ├── DataModels_CS.h             # — tidak berubah
│   ├── Network_EspNow.h            # ✅ + g_rawQueue extern (item #5)
│   ├── Network_Mqtt.h              # — tidak berubah
│   ├── Sensor_MPU.h                # — tidak berubah
│   ├── Sensor_PPG.h                # — tidak berubah
│   └── Watchdog.h                  # — tidak berubah
│
├── src/
│   ├── main.cpp                    # ✅ + taskSerialize gateway (item #5)
│   ├── CS_Sensor.cpp               # ✅ definisi static member singleton
│   ├── task_cs_sender.cpp          # ✅ printInfo() ditambah
│   ├── Network_EspNow.cpp          # ✅ ISR minimal (item #5), BatchBuffer dihapus
│   ├── Network_Mqtt.cpp            # — tidak berubah
│   ├── Sensor_MPU.cpp              # — tidak berubah
│   ├── Sensor_PPG.cpp              # — tidak berubah
│   └── Watchdog.cpp                # — tidak berubah
│   # 🗑 Network_EspNow_CS_handler.cpp → HAPUS dari project Anda
│
└── server/
    ├── __main__.py                 # ✅ petunjuk cara run
    ├── requirements.txt            # ✅
    ├── core/
    │   ├── __init__.py             # ✅
    │   ├── config.py               # ✅ semua parameter terpusat
    │   └── cs_utils.py             # ✅ generate_phi, reconstruct, singleton
    └── apps/
        ├── __init__.py             # ✅
        ├── reconstruct_server.py   # ✅
        ├── live_visualizer.py      # ✅
        └── test_single_signal.py   # ✅ label grafik diperbaiki
    # 🗑 cs_reconstruct_server.py → HAPUS dari project Anda
    # 🗑 cs_live_visualizer.py    → HAPUS dari project Anda
    # 🗑 tes.py                   → HAPUS dari project Anda
```

---

## Rencana Refactor — Status

### ✅ SELESAI

#### 1. Config Split
**Tujuan:** Pisah `Config.h` monolitik menjadi 4 file terorganisir agar orang awam cukup edit 1 file untuk setup.

**File yang dibuat:**
| File | Isi |
|---|---|
| `include/Config.h` | Titik masuk — hanya forward include, tidak perlu diubah |
| `include/config/credentials.h` | WiFi SSID/pass, MQTT broker IP/port/user/pass |
| `include/config/hardware.h` | Pin I2C, MAC address 3 ESP32, I2C address sensor |
| `include/config/features.h` | Flag on/off: finger gate, batching MQTT, topic base |
| `include/config/tuning.h` | Timing, task priority, stack size, queue size |
| `include/config/credentials.h.example` | Template kosong aman untuk di-commit ke git |
| `.gitignore` | Exclude credentials.h dari git |

**Cara migrasi:**
1. Hapus `include/Config.h` lama
2. Copy file-file baru ke `include/`
3. Semua `#include "Config.h"` di `.cpp` **tidak perlu diubah** — path sama

**Catatan teknis:**
- Namespace `Mqtt` di-split antara `credentials.h` dan `features.h` — valid di C++, merge otomatis saat compile, tidak ada nama yang duplikat

---

### ✅ SELESAI (lanjutan)

#### 2. CSPhiMatrix Singleton
**Tujuan:** Hemat ~48KB RAM di sensor node.

**Masalah saat ini:**
```cpp
// CS_Sensor.h — setiap CSEncoder simpan Φ sendiri
float _phi[CS_M][CS_N] = {};  // 32×64×4 = 8.192 byte per encoder
```
7 encoder × 8KB = **56KB** hanya untuk matrix Φ yang identik semua.

**Solusi:**
```cpp
// Baru: satu CSPhiMatrix singleton, semua encoder pakai pointer
class CSPhiMatrix {
    static float _phi[CS_M][CS_N]; // satu instance saja = 8KB
    static bool  _initialized;
public:
    static const float (*get())[CS_N];  // return pointer ke matrix
};

class CSEncoder {
    const float (*_phi)[CS_N]; // pointer ke singleton, bukan copy
    ...
};
```

**File yang perlu diubah:**
- `include/CS_Sensor.h` — pisah `CSPhiMatrix` dan `CSEncoder`, encoder terima pointer
- `src/task_cs_sender.cpp` — tidak berubah (API encoder sama)

**File yang dibuat/diubah:**
| File | Perubahan |
|---|---|
| `include/CS_Sensor.h` | Pisah `CSPhiMatrix` (singleton) dan `CSEncoder` (pakai pointer) |
| `src/CS_Sensor.cpp` | Baru — definisi static member `_phi[M][N]` dan `_initialized` |
| `src/task_cs_sender.cpp` | Tambah `CSPhiMatrix::printInfo()` di log awal, logika encode tidak berubah |

**Penghematan aktual:**
- Sebelum: 7 encoder × 8.192 byte = 57.344 byte untuk Φ
- Sesudah: 1 × 8.192 byte (singleton) + 7 × 4 byte (pointer) = 8.220 byte
- **Hemat: ~49KB heap**

**Cara migrasi:**
1. Ganti `include/CS_Sensor.h` lama dengan yang baru
2. Tambahkan `src/CS_Sensor.cpp` ke project (taruh di folder `src/`)
3. Ganti `src/task_cs_sender.cpp` dengan yang baru
4. Build — tidak ada perubahan API, semua kode yang pakai `CSEncoder` tetap sama

---

#### 3. Hapus File Duplikat ✅
**File yang dihapus/dipindah:**
- `src/Network_EspNow_CS_handler.cpp` → **hapus** (sudah ada di `Network_EspNow.cpp`)
- `server/tes.py` → **rename** ke `server/apps/test_single_signal.py`

**Aksi di project Anda:**
```bash
rm src/Network_EspNow_CS_handler.cpp
rm server/tes.py
rm server/cs_reconstruct_server.py
rm server/cs_live_visualizer.py
# (digantikan oleh versi baru di server/apps/)
```

---

#### 4. cs_utils.py — Shared Python Module ✅

**File yang dibuat:**
| File | Isi |
|---|---|
| `server/core/__init__.py` | Package marker |
| `server/core/config.py` | Semua parameter: CS_N, CS_M, MQTT_BROKER, LASSO_ALPHA, dll |
| `server/core/cs_utils.py` | `generate_phi()`, `reconstruct()`, `reconstruct_default()`, singleton PHI/THETA/PSI |
| `server/apps/__init__.py` | Package marker |
| `server/apps/reconstruct_server.py` | Pengganti `cs_reconstruct_server.py` |
| `server/apps/live_visualizer.py` | Pengganti `cs_live_visualizer.py` |
| `server/apps/test_single_signal.py` | Pengganti `tes.py` |
| `server/requirements.txt` | `pip install -r requirements.txt` |

**Cara jalankan setelah refactor:**
```bash
cd project_root/
pip install -r server/requirements.txt

python -m server.apps.reconstruct_server
python -m server.apps.live_visualizer
python -m server.apps.test_single_signal
```

---

#### 5. ISR Offload — onDataRecv ✅

**File yang diubah:**
| File | Perubahan |
|---|---|
| `include/DataModels.h` | Tambah `RawPacket` struct (250B data + 1B len + 6B mac = 257B) |
| `include/Network_EspNow.h` | Tambah `extern QueueHandle_t g_rawQueue` |
| `src/Network_EspNow.cpp` | ISR `onDataRecv` jadi minimal: hanya memcpy + xQueueSendFromISR + portYIELD_FROM_ISR. BatchBuffer dihapus (dipindah ke main.cpp) |
| `src/main.cpp` | Gateway: tambah `taskSerialize` (Core 1) — semua serialisasi JSON ada di sini. `g_rawQueue` dibuat sebelum `g_espnow.begin()` |
| `server/__main__.py` | Baru — petunjuk cara run jika `python -m server` diketik |

**Pipeline baru gateway:**
```
ISR onDataRecv  →  g_rawQueue  →  taskSerialize  →  g_mqttQueue  →  taskMqttPublish
    ~1µs                           snprintf JSON                      mqtt.publish()
    (aman)                         (Core 1)                           (Core 0)
```

**RAM g_rawQueue:** 257 × 10 = 2.57 KB (jauh lebih hemat dari serialize di ISR)

**Bug yang diperbaiki saat audit:**
- `BatchBuffer` double-definition dihapus dari `Network_EspNow.cpp`
- Label grafik `test_single_signal.py` diperbaiki: "Tren Akurasi" → "Tren Sparsity" + note bahwa corr tidak tersedia tanpa ground truth

---

---

## Konteks Penting untuk Sesi Berikutnya

### Hal-hal non-obvious yang mudah terlupakan

1. **Urutan init di `main.cpp` KRITIS** — Wire.begin() untuk MAX30102 HARUS setelah `esp_now_init()`. Kalau dibalik, channel ESP-NOW jadi kacau → NACK terus-terusan.

2. **Dua mutex terpisah untuk dua bus I2C** — `g_wire0Mutex` untuk Wire (PPG), `g_wire1Mutex` untuk Wire1 (IMU). Jangan digabung — PPG prio lebih tinggi dan akan starve IMU.

3. **Gateway pakai `WIFI_AP_STA`** — bukan pure STA. AP diperlukan untuk mengunci channel radio agar ESP-NOW stabil. AP-nya hidden dan tidak perlu diakses siapapun.

4. **Namespace `Mqtt` tersebar di 2 file config** — `credentials.h` (broker info) dan `features.h` (topic, keepalive). Ini disengaja agar pemisahan logis tetap jelas.

5. **CSEncoder seed HARUS sama dengan server Python** — `CS_PHI_SEED = 42` di `CS_Sensor.h` harus identik dengan `CS_PHI_SEED = 42` di semua script Python. Kalau beda, rekonstruksi akan gagal total (korelasi mendekati 0).

6. **`MqttMessage` payload dibatasi 420 byte** — sudah dihitung pas untuk `cs_ir` (~360B) + margin. Jangan naikkan sembarangan karena `30 × 500B = 15KB` heap yang sudah diperhitungkan ketat di gateway.

### Perintah PlatformIO berguna
```bash
# Upload per node
pio run -e node_sensor_a -t upload
pio run -e node_sensor_b -t upload
pio run -e node_gateway   -t upload

# Monitor serial
pio device monitor -e node_sensor_a
pio device monitor -e node_gateway

# Build saja (cek error tanpa upload)
pio run -e node_sensor_a
```

### Jalankan server Python
```bash
# Install dependencies (sekali saja)
pip install paho-mqtt numpy scikit-learn scipy matplotlib

# Terminal 1: rekonstruksi CS
python server/cs_reconstruct_server.py

# Terminal 2: visualisasi real-time
python server/cs_live_visualizer.py
```

---

## Checklist Sebelum Deploy

- [ ] `credentials.h` sudah diisi (SSID, password, IP broker)
- [ ] MAC address di `hardware.h` sudah sesuai 3 ESP32 yang dipakai
- [ ] Mosquitto broker berjalan: `mosquitto -v`
- [ ] PC dan ESP32 gateway terhubung ke WiFi yang sama
- [ ] Channel ESP-NOW sensor = channel WiFi gateway (cek log boot)
- [ ] `CS_PHI_SEED` di firmware sama dengan di Python server

---

---

## ✅ SEMUA REFACTOR SELESAI

Semua 5 item refactor sudah selesai. Project siap untuk penambahan fitur baru.

### Cara menambah fitur baru

**Fitur sensor baru** (misal sensor baru, sinyal baru):
1. Tambah struct di `include/DataModels.h` atau `include/DataModels_CS.h`
2. Buat `include/Sensor_XXX.h` + `src/Sensor_XXX.cpp` (ikuti pola Sensor_MPU)
3. Daftarkan task di `src/main.cpp` bagian `ROLE_SENSOR`

**Ubah parameter CS** (N, M, alpha):
1. `include/CS_Sensor.h` — ubah `CS_N`, `CS_M`, `CS_PHI_SEED`
2. `server/core/config.py` — ubah nilai yang sama
3. Compile ulang firmware + restart server Python

**Tambah output server baru** (misal InfluxDB, ML model):
1. Edit `server/apps/reconstruct_server.py` bagian `# TODO`
2. Semua sinyal rekonstruksi tersedia di `results` dict

*Terakhir diupdate: Semua refactor selesai ✅ — Config Split, CSPhiMatrix, Hapus duplikat, cs_utils.py, ISR Offload.*
