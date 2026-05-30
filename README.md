# 🩺 Health Monitor Mesh — ESP32 Multi-Hop Wireless Sensor Network

> **Sistem monitoring kesehatan real-time berbasis ESP32 dengan jaringan mesh ESP-NOW, Compressive Sensing, dan dynamic routing multi-hop ke broker MQTT.**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?logo=espressif)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%2B%20FreeRTOS-green)](https://docs.platformio.org/)
[![Build](https://img.shields.io/badge/build-PlatformIO-orange?logo=platformio)](https://platformio.org/)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)


---

## 📑 Table of Contents

- [Tentang Proyek](#-tentang-proyek)
- [Fitur Utama](#-fitur-utama)
- [Arsitektur Sistem](#-arsitektur-sistem)
- [Tech Stack & Hardware](#-tech-stack--hardware)
- [Quick Start](#-quick-start)
- [Konfigurasi](#️-konfigurasi)
- [Struktur Repositori](#-struktur-repositori)
- [Topologi Jaringan Mesh](#-topologi-jaringan-mesh)
- [Algoritma Compressive Sensing](#-algoritma-compressive-sensing)
- [FreeRTOS Task Layout](#-freertos-task-layout)
- [Format Paket ESP-NOW](#-format-paket-esp-now)
- [MQTT Topics](#-mqtt-topics)
- [Troubleshooting](#-troubleshooting)
- [Changelog](#-changelog)
- [Kontribusi](#-kontribusi)
- [Lisensi](#-lisensi)

---

## 🔬 Tentang Proyek

**Health Monitor Mesh** adalah firmware ESP32 untuk sistem monitoring kesehatan wearable yang mengukur sinyal IMU (accelerometer + gyroscope) dan PPG (photoplethysmography — detak jantung + SpO2) secara real-time.

Data sensor dikompresi menggunakan **Compressive Sensing (CS)** dengan matriks Hadamard-Gaussian sebelum dikirim melalui jaringan **ESP-NOW mesh multi-hop**. Gateway meneruskan data ke broker **MQTT** untuk diproses dan divisualisasikan lebih lanjut.

Sistem ini dirancang untuk:
- **Ketahanan jaringan** — jika sinyal sensor lemah ke gateway, data direlai melalui sensor tetangga
- **Efisiensi bandwidth** — kompresi 50% via CS (64 sampel → 32 pengukuran per window)
- **Stabilitas operasional** — hardware watchdog, heap monitor, dan restart otomatis

---

## ✨ Fitur Utama

| Fitur | Deskripsi |
|---|---|
| **Multi-Hop Routing** | Sensor memilih rute optimal (langsung / via relay) berdasarkan RSSI real-time |
| **Compressive Sensing** | Hadamard-Gaussian Φ, identik dengan server Python untuk rekonstruksi OMP |
| **WiFi Channel Sync** | Mendapatkan channel dengan konek singkat ke AP WiFi, lalu beralih ke ESP-NOW |
| **SpO2 Ratio-of-Ratios** | Algoritma Beer-Lambert dengan lookup table empiris Maxim AN6945 |
| **IMU Sanity Check** | Drop window jika ada axis IMU di luar batas fisis (±2.5g, ±300°/s) |
| **Hardware Watchdog** | Timeout 30 detik + heap monitor + stack watermark per task |
| **Restart Log (RTC)** | Alasan restart disimpan di RTC memory — bertahan setelah power cycle |
| **Dynamic RSSI Exchange** | Sensor saling bertukar info RSSI ke gateway setiap 2 detik |
| **Exponential Backoff** | Reconnect MQTT dengan backoff 5s → 60s saat broker down |
| **FreeRTOS Multi-Core** | Task IMU/PPG di Core 1, TX/MQTT di Core 0 — tidak saling blokir |

---

## 🏗️ Arsitektur Sistem

```
┌─────────────────────────────────────────────────────────────────────┐
│                        SENSOR NODE A (Node 1)                       │
│  MPU6050 ───► IMU Task ──►┐                                         │
│                           ├──► CS Encoder ──► ESP-NOW TX ──────────►│
│  MAX30102 ──► PPG Task ──►┘   (Φ·x = y)    Direct / Via Relay       │
└─────────────────────────────────────────────────────────────────────┘
                                                      │
                                         ┌────────────┴─────────────┐
                                         │   Multi-Hop Decision     │
                                         │  RSSI Self vs RSSI Relay │
                                         └────────────┬─────────────┘
                                                      │
┌─────────────────────────────────────────────────────────────────────┐
│                        SENSOR NODE B (Node 2)                       │
│  MPU6050 ───► IMU Task ──►┐                                         │
│                           ├──► CS Encoder ──► ESP-NOW TX ──────────►│
│  MAX30102 ──► PPG Task ──►┘              Relay jika diminta         │
└─────────────────────────────────────────────────────────────────────┘
                                                      │
┌─────────────────────────────────────────────────────▼───────────────┐
│                        GATEWAY NODE (Node 0)                        │
│  ESP-NOW RX ──► rawQueue ──► MeshRouting ──► mqttQueue              │
│  BeaconTask ◄── broadcast ch_lock (AP hidden)                       │
│  WiFi STA ──► MQTT Broker ──► Dashboard / Server Python             │
└─────────────────────────────────────────────────────────────────────┘
```

**Alur data lengkap:**

1. Sensor membaca IMU (100 Hz) dan PPG (100 Hz)
2. Setiap 64 sampel, CSEncoder menghitung `y = Φ · x` (32 float)
3. DynamicRouter memutuskan: kirim langsung ke gateway atau via relay
4. Gateway menerima, MeshRouting mengakumulasi 6 axis IMU + PPG per window
5. Setelah lengkap, payload JSON diterbitkan ke MQTT topic

---

## 🛠️ Tech Stack & Hardware

### Software

| Komponen | Detail |
|---|---|
| **Platform** | PlatformIO + Arduino Framework |
| **RTOS** | FreeRTOS (bawaan ESP32 Arduino) |
| **Transport** | ESP-NOW (IEEE 802.11 vendor-specific) |
| **Protokol IoT** | MQTT via PubSubClient |
| **Serialisasi** | ArduinoJson v7 |
| **CS Algorithm** | Hadamard-Gaussian + OMP (rekonstruksi di server) |

### Hardware per Node Sensor

| Komponen | Fungsi | Bus |
|---|---|---|
| **ESP32 DevKit** | Mikrokontroler utama | — |
| **MPU6050** | Accelerometer + Gyroscope (3-axis each) | I2C Wire1 (SDA=21, SCL=22) |
| **MAX30102** | PPG, Heart Rate, SpO2 | I2C Wire (SDA=18, SCL=19) |

### Hardware Gateway

| Komponen | Fungsi |
|---|---|
| **ESP32 DevKit** | Gateway ESP-NOW ↔ WiFi/MQTT |
| Broker MQTT | Mosquitto |

---

## 🚀 Quick Start

### Prerequisites

- [PlatformIO IDE for VSCode](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) atau **PlatformIO CLI**:
  ```bash
  pip install platformio
  ```
- **ESP32 DevKit** × 3 (2 sensor + 1 gateway)
- **MPU6050** × 2
- **MAX30102** × 2
- **Broker MQTT** (Mosquitto)

### 1. Clone Repositori

```bash
git https://github.com/dwaynimay/IoTProject.git
cd IoTProject
```

### 2. Konfigurasi Credentials

```bash
cp include/config/credentials.h.example include/config/credentials.h
```

Edit `include/config/credentials.h`:

```cpp
namespace Wifi {
    static constexpr char SSID[]     = "NAMA_WIFI";
    static constexpr char PASSWORD[] = "PASSWORD_WIFI";
}

namespace Mqtt {
    static constexpr char BROKER[]    = "IP_ADD_BROKER";
    static constexpr int  PORT        = 1883;
    static constexpr char CLIENT_ID[] = "esp32_gateway";
    static constexpr char USER[]      = "";
    static constexpr char PASSWORD[]  = "";
}
```

### 3. Konfigurasi MAC Address

Cek MAC address tiap ESP32 dengan sketch sederhana:

```cpp
#include <WiFi.h>
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    Serial.println(WiFi.macAddress());
}
void loop() {}
```

Lalu update `include/config/hardware.h`:

```cpp
namespace MacAddr {
    constexpr uint8_t NODE_A[6]  = {0xF4, 0x2D, 0xC9, 0x6F, 0x5C, 0x40};
    constexpr uint8_t NODE_B[6]  = {0x28, 0x05, 0xA5, 0x31, 0xF4, 0x94};
    constexpr uint8_t GATEWAY[6] = {0xF4, 0x2D, 0xC9, 0x70, 0xD1, 0x34};
}
```

### 4. Build & Upload

<!-- ```bash
# Upload ke Sensor Node A
pio run -e node_sensor_a --target upload

# Upload ke Sensor Node B
pio run -e node_sensor_b --target upload

# Upload ke Gateway
pio run -e node_gateway --target upload
``` -->

### 5. Monitor Serial

<!-- ```bash
pio device monitor --baud 115200
``` -->
<!-- 
Output normal di sensor:
```
[   500ms] [INFO ] [MAIN] Health Monitor Mesh v3.1 — Multi-Hop Routing
[   501ms] [INFO ] [MAIN] Node 1 | Role: SENSOR
[   800ms] [INFO ] [MPU ] MPU6050 siap | Wire1 pin SDA=21 SCL=22
[  1200ms] [INFO ] [MESH] WiFi connected! ch=6 | IP=192.168.1.45 | RSSI=-52 dBm
[  1201ms] [INFO ] [MESH] Mode: SENSOR | ch=6
[  7000ms] [INFO ] [CS_TX] Gateway terdeteksi — mulai encode & kirim data
[  7640ms] [INFO ] [CS_TX] Win #1 [DIRECT] | self=-52 dBm | relay=0.0% | HR=0 SpO2=0.0%
``` -->

---

## ⚙️ Konfigurasi

Semua parameter konfigurasi terpusat di folder `include/config/`:

### `features.h` — Log Level & Fitur

```cpp
#define LOG_LEVEL        3    // 0=SILENT 1=ERROR 2=WARN 3=INFO 4=DEBUG
#define LOG_ENABLE_COLOR 0    // 1 untuk terminal berwarna (ANSI)
```

### `tuning.h` — Timing & Performa

| Parameter | Default | Keterangan |
|---|---|---|
| `Timing::SEND_INTERVAL_MS` | 200 ms | Interval kirim data (5 Hz) |
| `Timing::IMU_SAMPLE_MS` | 10 ms | Sampling IMU internal (100 Hz) |
| `RoutingCfg::RELAY_THRESHOLD_DBM` | 5 dBm | Margin minimum untuk pilih relay |
| `RoutingCfg::RSSI_STALE_MS` | 10000 ms | Timeout RSSI sebelum dianggap stale |
| `RoutingCfg::DISCOVERY_PHASE_MS` | 6000 ms | Fase discovery sebelum kirim data |

### `tuning.h` — Topologi Mesh

Untuk mengaktifkan multi-hop (3-node mesh):

```cpp
namespace MeshTopology {
    static constexpr uint8_t nodeNeighbors[3][2] = {
        {0, 0},  // Node 0 (GATEWAY): tidak ada relay
        {2, 0},  // Node 1: relay via Node 2 jika lebih baik
        {1, 0}   // Node 2: relay via Node 1 jika lebih baik
    };
    static constexpr uint8_t maxNeighborsPerNode = 2;
    static constexpr uint8_t totalNodes = 3;
}
```

Untuk star topology (2-node, tanpa relay):

```cpp
// Uncomment bagian 2-node di tuning.h dan comment bagian 3-node
static constexpr uint8_t nodeNeighbors[3][1] = {
    {0}, {0}, {0}  // semua langsung ke gateway
};
```

---

## 📁 Struktur Repositori

```
firmware/
├── include/
│   ├── Config.h                    # Entry point konfigurasi tunggal
│   ├── config/
│   │   ├── credentials.h.example   # Template credentials
│   │   ├── features.h              # Log level & fitur on/off
│   │   ├── hardware.h              # Pin, MAC address, I2C addr
│   │   └── tuning.h                # Timing, priority, mesh topology
│   └── utils/
│       └── Logger.h                # Makro LOG_INFO/WARN/ERROR/DEBUG
│
├── lib/
│   ├── CS_Model_Gaussian/
│   │   ├── CS_Sensor.h             # CSPhiMatrix (singleton) + CSEncoder
│   │   └── CS_Sensor.cpp           # Definisi static member
│   ├── EspNowMesh/
│   │   ├── EspNowMesh.h            # Transport layer ESP-NOW
│   │   ├── EspNowMesh.cpp          # WiFi channel sync + send/recv
│   │   ├── MeshPackets.h           # Definisi semua struct paket
│   │   ├── MeshRouting.cpp
│   │   └── MeshRouting.h           # Dispatch paket → MQTT topic
│   ├── HealthSensors/
│   │   ├── Sensor_MPU.h
│   │   ├── Sensor_MPU.cpp          # Driver MPU6050 (raw I2C)
│   │   ├── Sensor_PPG.h
│   │   └── Sensor_MPU.cpp          # Driver MAX30102 + SpO2
│   ├── Network_Mqtt/
│   │   ├── Network_Mqtt.h
│   │   └── Network_Mqtt.cpp        # WiFi AP_STA + MQTT + reconnect
│   ├── Routing/
│   │   ├── DynamicRouter.h
│   │   └── DynamicRouter.cpp       # RSSI-based routing decision engine
│   └── Watchdog/
│   │   ├── Watchdog.h
│       └── Watchdog/.cpp           # HW WDT + heap/stack monitor
│
├── src/
│   ├── main.cpp                    # Inisialisasi & FreeRTOS task creation
│   ├── task_cs_sender.cpp          # Encode CS + kirim via ESP-NOW
│   └── task_mesh_handler.cpp       # Terima raw packet + publish MQTT
│
└── platformio.ini                  # Build environments (sensor_a/b, gateway)
```

---

## 🌐 Topologi Jaringan Mesh

### Protokol Komunikasi ESP-NOW

```text
[ Sensor Node ] ────── (ESP-NOW) ─────▶ [ Gateway Node ]
  MAC: Statis                              MAC: Statis
  CH: WiFi-synced                          CH: WiFi (persistent)
```


| # | Requirement | Keterangan |
|---|---|---|
| 01 | MAC Address | MAC tujuan harus diketahui sebelum kirim |
| 02 | Channel sama | Sender & receiver wajib di channel identik · `WiFi-bootstrapped` |
| 03 | Peer registered | MAC tujuan wajib didaftarkan · `esp_now_add_peer()` |
### ESP-NOW Multi-Hop

```
[Sensor A] ←→ [Sensor B] ←→ [Gateway]
    │                           ▲
    └───────────────────────────┘
          (fallback direct)

Keputusan routing per window:
  RSSI_neighbor - RSSI_self >= 5 dBm  → RELAY
  otherwise                           → DIRECT
```

### Skenario Routing

| Kondisi | Keputusan | Alasan |
|---|---|---|
| RSSI self = -55, neighbor = -45 | RELAY | Neighbor lebih baik 10 dBm |
| RSSI self = -55, neighbor = -58 | DIRECT | Self lebih baik |
| RSSI self = -55, neighbor = -52 | DIRECT | Beda < 5 dBm (threshold) |
| RSSI neighbor stale (>10 detik) | DIRECT | Fallback safety |
| Discovery phase (<6 detik boot) | DIRECT | Belum cukup data |

### Channel Synchronization

Sensor otomatis sync channel ke gateway via WiFi association:

```
Boot sensor:
  1. WiFi.begin(SSID, PASSWORD)    ← konek ke AP yang sama dengan gateway
  2. ch = WiFi.channel()           ← baca channel dari AP
  3. WiFi.disconnect()             ← keluar WiFi
  4. esp_wifi_set_channel(ch)      ← set channel untuk ESP-NOW
  5. esp_now_init()                ← init di channel yang benar
```

---

## 📐 Algoritma Compressive Sensing

Sistem mengimplementasikan CS pipeline untuk efisiensi transmisi:

```
SENSOR (ESP32)                   SERVER (Python)
─────────────                    ───────────────
x ∈ ℝ^64  (window)               y ∈ ℝ^32  (terima dari MQTT)
     │                                  │
     ▼                                  ▼
y = Φ · x                        x̂ = OMP(y, Φ, Ψ)
Φ: Hadamard-Gaussian             Ψ: Fourier basis
32×64 matrix                     Rekonstruksi sparse signal
     │
     ▼
Kirim 32 float via ESP-NOW
(hemat 50% bandwidth)
```

### Parameter CS

| Parameter | Nilai | Keterangan |
|---|---|---|
| `CS_N` | 64 | Panjang window (harus pangkat 2) |
| `CS_M` | 32 | Jumlah pengukuran (rasio kompresi 50%) |
| `CS_PHI_SEED` | 0 | Seed LCG — **harus sama dengan server Python** |

### Sinkronisasi dengan Server

Verifikasi Φ antara ESP32 dan server Python:

```bash
# Di ESP32 Serial Monitor, akan tercetak saat boot:
# [INFO] [CS] PHI[0][0..7]: 0.044194 -0.044194 0.044194 ...
# [INFO] [CS] norm(PHI[0]) = 0.176777 (harus 0.176777)

# Di server Python:
python -m server.verify_phi
```

Kedua output harus identik. Jika berbeda, cek `CS_PHI_SEED` dan konstanta LCG.

---

## 🧵 FreeRTOS Task Layout

### Sensor Node

| Task | Core | Priority | Stack | Fungsi |
|---|---|---|---|---|
| `taskReadPPG` | 1 | 4 (tertinggi) | 4 KB | Poll MAX30102, update SpO2 |
| `taskReadIMU` | 1 | 3 | 4 KB | Read MPU6050 @ 100 Hz |
| `taskCSSender` | 0 | 2 | 12 KB | CS encode + ESP-NOW TX |
| `taskRssiExchange` | 0 | 1 | 4 KB | Kirim RSSI report ke neighbor |
| `taskMonitor` | 0 | 1 | 4 KB | Health check + watchdog feed |

### Gateway Node

| Task | Core | Priority | Stack | Fungsi |
|---|---|---|---|---|
| `taskBeacon` | 0 | 1 | 4 KB | Broadcast beacon setiap 1 detik |
| `taskMeshHandler` | 1 | 3 | 8 KB | Decode ESP-NOW → mqttQueue |
| `taskMqttPublish` | 0 | 2 | 8 KB | Drain mqttQueue → MQTT broker |
| `taskMonitor` | 0 | 1 | 4 KB | Monitor queue + WiFi + heap |

---

## 📦 Format Paket ESP-NOW

### Ukuran Paket (semua < 250 byte, limit ESP-NOW)

| Tipe | ID | Ukuran | Keterangan |
|---|---|---|---|
| `BEACON` | 0x01 | 7 B | Gateway → broadcast |
| `RSSI_REPORT` | 0x02 | 9 B | Node ↔ Node |
| `CS_AX`..`CS_GZ` | 0x10–0x15 | 136 B | 1 axis IMU CS (32 float) |
| `CS_IR` | 0x16 | 142 B | PPG CS + HR + SpO2 |
| `ROUTED_CS` | 0x20 | max 150 B | Wrapper multi-hop relay |
| `COMBINED_DATA` | 0x03 | 50 B | Raw IMU + PPG (legacy) |
| `HEARTBEAT` | 0xFF | 11 B | Uptime + RSSI report |

---

## 📡 MQTT Topics

Semua topic berada di bawah prefix `health_monitor/` (konfigurasi di `features.h`):

| Topic | Trigger | Payload |
|---|---|---|
| `health_monitor/node_{id}/cs_imu` | Setiap window (6 axis terkumpul) | JSON: `ts`, `ax[]`, `ay[]`, ..., `gz[]`, `finger` |
| `health_monitor/node_{id}/cs_ppg` | Setiap window PPG | JSON: `ts`, `hr`, `spo2`, `ir[]`, `ppg_valid`, `finger` |
| `health_monitor/gateway/status` | Saat konek/putus | `"online"` / `"offline"` (retain) |
---

## 🔧 Troubleshooting

### NACK tinggi saat transmisi

```
[WARN] [MESH] NACK total=45 rate=4.3%
```

**Penyebab & solusi:**
- Channel mismatch antara sensor dan gateway → pastikan WiFi SSID/password benar di `credentials.h`
- Jarak terlalu jauh → cek nilai RSSI di log `[CS_TX] Win #N [DIRECT] | self=X dBm`
- Collision antar node → stagger transmisi sudah diterapkan; naikkan `NODE_STAGGER_MS` di `task_cs_sender.cpp` jika masih terjadi

### Sensor tidak terdeteksi

```
[ERROR] [MPU] Tidak ada respons I2C (err=2). Cek wiring pin 21/22
[ERROR] [PPG] MAX30102 tidak ditemukan setelah 3x retry
```

**Solusi:**
- Periksa koneksi kabel SDA/SCL sesuai pin di `hardware.h`
- Verifikasi power supply (3.3V)
- MPU6050 kloningan: pastikan resistor pull-up 4.7kΩ terpasang di bus I2C
- MAX30102: Wire.begin() **harus** dipanggil setelah `esp_now_init()` (lihat urutan di `main.cpp`)

### IMU sanity check terus gagal

```
[WARN] [CS_TX] DROPPED: IMU out of range | ax=156.3 ay=-200.1 az=98.2 m/s²
```

**Penyebab:**
- Sensor MPU6050 mengembalikan nilai ekstrem (biasa terjadi pada kloningan) → jalankan `imu.calibrate()` sekali saat sensor diam
- Offset kalibrasi belum diset → nilai raw ADC belum dikurangi bias

### Heap kritis & restart otomatis

```
[ERROR] [WDT] Heap kritis: 3 KB < 4 KB — trigger restart!
```

**Solusi:**
- Kurangi `QueueLen::MQTT_MSG` di `tuning.h` (setiap -10 entry hemat ~19 KB)
- Kurangi `LOG_LEVEL` ke 2 (WARN) untuk produksi
- Pastikan `lib_ignore = CS_Model_Lasso` aktif di `platformio.ini` (hanya pakai satu CS model)

### MQTT tidak terhubung

```
[ERROR] [MQTT] MQTT gagal | state=-2 (FAILED)
[WARN ] [MQTT] Reconnect gagal — next retry dalam 10000 ms
```

**Solusi:**
- Verifikasi IP broker di `credentials.h`
- Cek apakah broker berjalan: `mosquitto -v` atau `netstat -an | grep 1883`
- Pastikan `CLIENT_ID` unik jika ada beberapa instance gateway

---

## 🤝 Kontribusi

Kontribusi sangat diterima! Ikuti langkah berikut:

1. **Fork** repositori ini
2. **Buat branch** untuk fitur atau fix Anda:
   ```bash
   git checkout -b feat/nama-fitur
   ```
3. **Setup environment** — ikuti Quick Start di atas
4. **Ikuti aturan kode:**
   - Semua log wajib menggunakan `LOG_*` makro dari `Logger.h`, **bukan** `Serial.print` langsung
   - Konstanta baru masuk ke namespace yang sesuai di `tuning.h` atau `hardware.h`
   - File baru wajib punya header komentar dengan tanggung jawab modul
5. **Commit** dengan pesan yang deskriptif:
   ```
   feat: tambah retry logic untuk RSSI exchange
   fix: perbaiki race condition di _promiscuousRxCb
   docs: update topologi mesh di README
   ```
6. **Buka Pull Request** dengan:
   - Deskripsi singkat perubahan
   - Output Serial Monitor sebelum/sesudah (jika relevan)
   - Nomor issue terkait (jika ada)

---