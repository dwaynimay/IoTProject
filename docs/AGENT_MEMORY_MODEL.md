# Agent Memory Model - IoTProject

Dokumen ini dibuat sebagai memori kerja untuk agent agar cepat memahami proyek TA tanpa harus membaca ulang seluruh repositori. Fokusnya adalah menjelaskan konteks proyek, tujuan sistem, stack yang dipakai, alur kerja fitur, struktur modul, dan detail implementasi penting yang berpengaruh ke pengembangan, debugging, analisis hasil, dan penyusunan naskah TA.

## 1. Identitas Proyek

- Nama proyek: `Health Monitor Mesh`
- Domain: IoT kesehatan / wearable sensing / wireless sensor network
- Bentuk sistem: sistem monitoring kesehatan real-time berbasis node ESP32 dan server Python
- Tujuan utama:
  - membaca data IMU dan PPG dari node sensor
  - mengompresi data menggunakan Compressive Sensing
  - mengirim data secara nirkabel memakai ESP-NOW mesh multi-hop
  - meneruskan data melalui gateway ke MQTT broker
  - merekonstruksi sinyal di server
  - menilai kualitas sinyal
  - menjalankan inferensi machine learning
  - menampilkan hasil secara live di dashboard

## 2. Gambaran Sistem End-to-End

Sistem dibagi menjadi dua lapisan besar:

1. `firmware/`
   Menjalankan logika embedded pada ESP32 untuk sensor node dan gateway.

2. `server/`
   Menjalankan backend Python untuk MQTT listener, rekonstruksi sinyal, quality assessment, penyimpanan SQLite, REST API, WebSocket, dashboard, dan ML inference.

Alur data utamanya:

1. Node IMU membaca sensor MPU6050.
2. Node PPG membaca sensor MAX30102.
3. Data dibentuk per window dengan panjang `CS_N = 64`.
4. Setiap window dikompresi menjadi `CS_M = 32` measurement menggunakan matriks Hadamard.
5. Node sensor memilih rute `direct` atau `relay` berdasarkan RSSI.
6. Data dikirim ke gateway lewat ESP-NOW.
7. Gateway mengubah paket menjadi payload MQTT per topik `cs_imu` atau `cs_ppg`.
8. Server subscribe MQTT, memasangkan data IMU dan PPG ke dalam satu group logis.
9. Server merekonstruksi sinyal kembali ke domain waktu.
10. Server menghitung kualitas sinyal, menyimpan hasil ke SQLite, lalu mendorong hasil ke dashboard via WebSocket.
11. Model ML membaca hasil window untuk klasifikasi, misalnya deteksi jatuh dan deteksi stres.

## 3. Masalah yang Diselesaikan Proyek

Secara konseptual proyek ini menyasar beberapa masalah sekaligus:

- keterbatasan bandwidth pada transmisi data sensor kontinu
- jangkauan terbatas jika node sensor tidak selalu dekat gateway
- kebutuhan monitoring real-time yang tetap ringan di perangkat edge
- kebutuhan sinkronisasi antara data IMU dan PPG yang berasal dari node fisik berbeda
- kebutuhan analitik lanjutan di sisi server tanpa membebani firmware

Pendekatan solusi:

- Compressive Sensing untuk mengurangi jumlah data yang dikirim
- ESP-NOW untuk komunikasi lokal yang ringan
- mesh multi-hop untuk ketahanan rute
- MQTT untuk integrasi gateway ke server
- rekonstruksi dan analitik dilakukan di server Python

## 4. Stack dan Platform yang Digunakan

### 4.1 Firmware / Embedded

- Mikrokontroler: `ESP32 DevKit`
- Framework: `Arduino Framework`
- Build system: `PlatformIO`
- RTOS: `FreeRTOS` bawaan ekosistem ESP32 Arduino
- Komunikasi wireless lokal: `ESP-NOW`
- Backhaul gateway: `WiFi + MQTT`
- Library utama:
  - `SparkFun MAX3010x Pulse and Proximity Sensor Library`
  - `PubSubClient`
  - `ArduinoJson`

### 4.2 Sensor dan Hardware

- IMU: `MPU6050`
- PPG: `MAX30102`
- Bus komunikasi sensor: `I2C`
- Konfigurasi pin:
  - node PPG: `SDA=18`, `SCL=19`, interrupt `PPG_INT=23`
  - node IMU: `SDA=21`, `SCL=22`
- Clock I2C: `400 kHz`

### 4.3 Server / Backend

- Bahasa: `Python`
- Runtime minimal yang ditargetkan tooling: `Python 3.10+`
- Web framework: `FastAPI`
- ASGI server: `uvicorn`
- MQTT client: `paho-mqtt`
- Storage: `SQLite`
- Numeric processing: `numpy`
- Format konfigurasi model: `JSON`
- Model serialization: `pickle (.pkl)`
- Testing: `pytest`
- Formatting/linting:
  - `black`
  - `ruff`
  - `mypy`

### 4.4 Frontend Dashboard

- Dashboard statis di `server/static/`
- Komunikasi live: `WebSocket`
- API tambahan: `REST`
- Teknologi frontend: HTML, CSS, JavaScript modular sederhana

## 5. Arsitektur Fisik Node

Repositori saat ini memakai 3 peran node:

- `node_imu`
  - `NODE_ROLE=ROLE_SENSOR_IMU`
  - `NODE_ID=1`
  - fokus mengirim `cs_imu`

- `node_ppg`
  - `NODE_ROLE=ROLE_SENSOR_PPG`
  - `NODE_ID=2`
  - fokus mengirim `cs_ppg`

- `node_gateway`
  - `NODE_ROLE=ROLE_GATEWAY`
  - `NODE_ID=3`
  - menerima paket ESP-NOW dan mem-publish ke MQTT

Catatan penting:

- Dari sisi firmware, IMU dan PPG berjalan pada node fisik berbeda.
- Dari sisi server, keduanya dipasangkan menjadi satu entitas logis lewat `NODE_GROUPS`.
- Mapping default saat ini:
  - group `1` = `imu_node=1` dan `ppg_node=2`

Ini berarti satu "subjek monitoring" dibentuk dari dua sumber fisik berbeda.

## 6. Konsep Kerja Fitur Inti

### 6.1 Akuisisi Data Sensor

Firmware membaca sensor secara synchronous di dalam task pengiriman, bukan lewat task baca sensor terpisah.

- IMU:
  - dibaca dari `SensorMPU`
  - sampling internal `10 ms` atau `100 Hz`
- PPG:
  - dibaca dari `SensorPPG`
  - update dan read dilakukan kontinu

Konsekuensi desain:

- pipeline lebih sederhana
- data langsung masuk encoder
- perlu mutex `g_wireMutex` untuk akses I2C yang aman

### 6.2 Compressive Sensing

Konsep:

- panjang window asli `N = 64`
- hasil kompresi `M = 32`
- rasio kompresi `50%`
- matriks sensing memakai varian Hadamard dengan seed tetap

Implementasi:

- firmware: `firmware/lib/CS_Model_Hadamard/`
- server: `server/cs/`

Aturan penting:

- `CS_N`, `CS_M`, dan `CS_PHI_SEED` harus konsisten antara firmware dan server
- server melakukan rekonstruksi dari measurement `y` menjadi sinyal estimasi `x_hat`

Makna akademik untuk TA:

- CS dipakai untuk efisiensi transmisi
- beban rekonstruksi dipindahkan ke server
- proyek ini bukan sekadar sensing, tetapi juga optimasi komunikasi data

### 6.3 Dynamic Routing dan Mesh Multi-Hop

Node sensor tidak selalu mengirim langsung ke gateway. Ia dapat memilih relay berdasarkan kualitas link.

Komponen utama:

- `DynamicRouter`
- `EspNowMesh`
- `MeshRouting`

Prinsip keputusan:

- node menyimpan RSSI dirinya ke gateway
- node juga menerima RSSI tetangga ke gateway
- jika neighbor lebih baik melebihi threshold, data boleh dikirim via relay

Parameter penting:

- `RoutingCfg::RELAY_THRESHOLD_DBM`
- `RoutingCfg::RSSI_EXCHANGE_MS`
- `RoutingCfg::DISCOVERY_PHASE_MS`

Topologi saat ini di `firmware/include/config/tuning.h`:

- Node 1 dapat memakai Node 2 sebagai relay
- Node 2 dapat memakai Node 1 sebagai relay

### 6.4 Sinkronisasi Channel WiFi / ESP-NOW

Salah satu hal penting di proyek ini adalah sinkronisasi channel.

Masalah:

- ESP-NOW harus bekerja pada channel yang sama
- gateway memakai WiFi aktif untuk MQTT
- jika urutan inisialisasi salah, ESP-NOW bisa crash atau tidak terkoneksi

Solusi desain:

- gateway:
  1. konek WiFi/MQTT dulu
  2. baru init ESP-NOW
  3. lalu lock channel gateway berdasarkan channel WiFi aktif

- sensor:
  - `g_mesh.begin(true)` dibuat non-blocking
  - background discovery mencari channel yang benar
  - task pengiriman menunggu `isChannelConfirmed()`

Ini salah satu detail teknis yang penting untuk dijelaskan di TA karena sangat menentukan stabilitas sistem.

### 6.5 Gateway to MQTT Bridge

Gateway bertugas sebagai jembatan antara domain ESP-NOW dan domain IP/MQTT.

Task inti gateway:

- `taskBeacon`
  - broadcast beacon untuk discovery dan RSSI
- `taskMeshHandler`
  - baca raw packet
  - route dan akumulasi per jenis paket
  - ubah menjadi `MqttMessage`
- `taskMqttPublish`
  - kirim pesan ke broker MQTT
  - menangani reconnect
  - menjaga queue agar tidak overflow
- `taskMonitorGateway`
  - cek kesehatan sistem, queue, heap, WiFi

### 6.6 Rekonstruksi Sinyal di Server

Setelah data `cs_imu` dan `cs_ppg` diterima server:

1. payload MQTT diparse
2. data dialihkan ke `NodeState`
3. jika pasangan IMU dan PPG untuk satu group sudah lengkap, `process_window()` dipanggil
4. server menjalankan `reconstruct(y)` untuk tiap sinyal
5. mean yang dikirim dari firmware ditambahkan kembali ke sinyal hasil rekonstruksi

Sinyal yang direkonstruksi:

- IMU:
  - `ax`, `ay`, `az`, `gx`, `gy`, `gz`
- PPG:
  - `ir`

### 6.7 Quality Assessment

Server tidak hanya merekonstruksi, tetapi juga memberi penilaian kualitas sinyal.

Komponen:

- `server/core/quality.py`
- `QualityAssessor`

Output kualitas dipakai untuk:

- penanda window `LOW_QUALITY`
- penanda window `CRITICAL`
- logging event ke SQLite
- notifikasi ke dashboard

### 6.8 Machine Learning Inference

ML di proyek ini dibuat generic, sehingga model bisa diganti tanpa mengubah engine.

Arsitektur:

- `ModelRegistry` memuat model dari folder model
- `MLInferenceEngine` menangani load, validasi, ekstraksi fitur, dan `predict_proba`
- `FeatureExtractor` membentuk feature vector dari `WindowInput`

Kontrak model:

- file model: `.pkl`
- file konfigurasi: `.json`
- model wajib punya `predict_proba`

Model yang sudah ada:

- IMU:
  - fall detection SVM
- PPG:
  - stress detection SVM

Fitur penting desain ini:

- model training dan runtime inference dipisah rapi
- engine tidak hardcode fitur tertentu
- manifest model menjadi kontrak antarlapisan

### 6.9 Dashboard Real-Time

Dashboard menerima data via WebSocket dari hub broadcast.

Fungsi dashboard:

- menampilkan stream window terbaru
- menampilkan status node
- menampilkan hasil kualitas sinyal
- menampilkan keluaran ML
- menyediakan endpoint API untuk inspeksi data historis dan maintenance

## 7. Struktur Kode yang Perlu Diingat

### 7.1 Firmware

- `firmware/src/main.cpp`
  - entry point firmware
  - inisialisasi role node
  - membuat task FreeRTOS

- `firmware/src/task_cs_sender.cpp`
  - pembacaan sensor
  - sanity check
  - encoding CS
  - dynamic routing
  - pengiriman ESP-NOW

- `firmware/src/task_mesh_handler.cpp`
  - pipeline gateway dari raw packet ke MQTT

- `firmware/lib/EspNowMesh/`
  - transport ESP-NOW

- `firmware/lib/Routing/`
  - logika routing dan pemilihan relay

- `firmware/lib/Network_Mqtt/`
  - WiFi dan MQTT untuk gateway

- `firmware/lib/HealthSensors/`
  - driver IMU dan PPG

- `firmware/lib/CS_Model_Hadamard/`
  - encoder dan matriks sensing di sisi firmware

### 7.2 Server

- `server/__main__.py`
  - entry point Python server

- `server/apps/main_app.py`
  - mode all-in-one: FastAPI + MQTT worker thread dalam satu proses

- `server/apps/reconstruct/listener.py`
  - subscriber MQTT
  - mapping physical node ke logical group

- `server/apps/reconstruct/node_state.py`
  - state buffer per group/node
  - sinkronisasi kedatangan IMU dan PPG

- `server/apps/reconstruct/processor.py`
  - rekonstruksi
  - quality assessment
  - storage
  - ML
  - notifikasi dashboard

- `server/apps/dashboard/`
  - API, route, WebSocket hub

- `server/apps/ml_inference/`
  - engine ML generik, feature extractor, registry, model

- `server/core/`
  - config, storage, validator, quality, logger

- `server/cs/`
  - algoritma rekonstruksi CS

## 8. Konfigurasi Penting

### 8.1 Firmware

File inti:

- `firmware/include/Config.h`
- `firmware/include/config/features.h`
- `firmware/include/config/credentials.h`
- `firmware/include/config/hardware.h`
- `firmware/include/config/tuning.h`

Yang wajib dijaga konsisten:

- role dan node ID dari `platformio.ini`
- MAC address antar node
- SSID WiFi dan broker MQTT
- parameter CS
- topologi neighbor mesh

### 8.2 Server

Konfigurasi dibaca dari environment variable melalui `server/core/config.py`.

Nilai penting:

- `MQTT_BROKER`
- `MQTT_PORT`
- `TOPIC_BASE`
- `DB_PATH`
- `RETENTION_HOURS`
- `CS_ALGORITHM`
- `LOG_LEVEL`
- `NODE_GROUPS`

## 9. Mode Operasi dan Entry Point

### 9.1 Firmware Build Environments

Environment penting di `firmware/platformio.ini`:

- produksi:
  - `node_imu`
  - `node_ppg`
  - `node_gateway`

- pengujian:
  - `test_ppg`
  - `test_imu`
  - `test_cs_compression`
  - `test_mesh_auto_gateway`
  - `test_mesh_auto_sensor_n1`
  - `test_mesh_auto_sensor_n2`
  - `test_e2e_gateway`
  - `test_e2e_sensor_n1`
  - `test_e2e_sensor_n2`
  - `test_imu_raw_vs_cs`
  - `test_ppg_raw_vs_cs`

Maknanya:

- repo ini tidak hanya berisi sistem final
- repo juga menyimpan skenario eksperimen dan pengujian terstruktur untuk kebutuhan TA

### 9.2 Server Runtime

Cara jalan utama:

```bash
python -m server
```

Ini akan:

1. membuka storage SQLite
2. menyiapkan event loop untuk hub WebSocket
3. memindai model ML pada `server/apps/ml_inference/models/`
4. menjalankan MQTT worker thread
5. membuka API dan dashboard pada port `8000`

## 10. Validasi, Pengujian, dan Artefak TA

Repo sudah memiliki banyak artefak eksperimen yang penting untuk penulisan TA:

- `docs/`
  - draft bab pengujian
  - panduan sumber data
  - gambar breadboard, skematik, prototype

- `server/data/`
  - hasil capture CS IMU
  - hasil capture CS PPG
  - hasil pengujian mesh RSSI
  - hasil pengujian end-to-end
  - backup hasil pengujian

- `server/tools/`
  - script capture dan analisis pengujian
  - builder dokumen Bab 5
  - simulator MQTT
  - visualizer live
  - verifikasi matriks Phi

- `server/tests/`
  - test validator
  - test storage
  - test quality
  - test engine ML
  - test full pipeline
  - test konsistensi Hadamard/CS

Kesimpulan penting:

- proyek ini sudah sangat dekat dengan kebutuhan dokumentasi TA karena data eksperimen dan tooling analisis sudah tersedia di repo

## 11. Asumsi Arsitektur yang Harus Diingat Agent

- firmware dan server saling bergantung pada parameter CS yang sama
- node IMU dan node PPG adalah perangkat fisik berbeda
- satu subjek monitoring dibentuk lewat group mapping di server
- gateway adalah satu-satunya penghubung dari ESP-NOW ke MQTT
- dashboard bukan sumber data utama, hanya lapisan presentasi
- ML inference berjalan di server, bukan di ESP32
- fokus kualitas ilmiah proyek ada pada kombinasi:
  - efisiensi transmisi CS
  - ketahanan komunikasi mesh
  - rekonstruksi sinyal
  - pemanfaatan data untuk inferensi kesehatan/aktivitas

## 12. Risiko Teknis dan Titik Sensitif

Hal-hal yang sering sensitif saat pengembangan:

- channel WiFi dan ESP-NOW tidak sinkron
- MAC address salah
- `CS_N`, `CS_M`, atau seed Phi tidak konsisten
- node group mapping server tidak sesuai firmware aktual
- MQTT queue gateway penuh saat koneksi broker buruk
- kualitas sensor PPG jelek saat finger tidak terdeteksi
- data IMU outlier menyebabkan window di-drop
- model ML tidak cocok dengan schema fitur di config

## 13. Bahasa Penjelasan yang Cocok untuk TA

Jika agent membantu menulis TA, proyek ini sebaiknya dijelaskan sebagai:

- sistem monitoring kesehatan berbasis wireless sensor network
- memakai pendekatan edge sensing dan server-side reconstruction
- menggabungkan optimasi komunikasi, pengolahan sinyal, dan machine learning

Kata kunci akademik yang relevan:

- Internet of Things
- wearable health monitoring
- ESP-NOW mesh
- multi-hop routing
- RSSI-based routing
- compressive sensing
- Hadamard sensing matrix
- Orthogonal Matching Pursuit
- signal reconstruction
- signal quality assessment
- real-time dashboard
- machine learning inference

## 14. Ringkasan Singkat untuk Agent

Jika harus mengingat proyek ini dalam bentuk sangat ringkas:

> Ini adalah proyek TA monitoring kesehatan real-time berbasis 3 ESP32, di mana node IMU dan node PPG mengirim data hasil Compressive Sensing melalui ESP-NOW mesh ke gateway, lalu gateway publish ke MQTT, server Python merekonstruksi sinyal, menilai kualitas, menjalankan ML inference, menyimpan ke SQLite, dan menampilkan hasil ke dashboard live.

## 15. File Referensi Awal yang Paling Penting

Jika nanti perlu refresh cepat, baca urutan ini:

1. `README.md`
2. `firmware/platformio.ini`
3. `firmware/src/main.cpp`
4. `firmware/src/task_cs_sender.cpp`
5. `firmware/src/task_mesh_handler.cpp`
6. `server/apps/main_app.py`
7. `server/apps/reconstruct/listener.py`
8. `server/apps/reconstruct/processor.py`
9. `server/core/config.py`
10. `server/apps/ml_inference/engine.py`

## 16. Tujuan Dokumen Ini

Dokumen ini dimaksudkan agar agent:

- cepat paham konteks proyek tanpa membaca semua file dari nol
- tidak keliru membedakan peran firmware, gateway, server, dan dashboard
- bisa membantu penulisan TA, analisis hasil, debugging, dan pengembangan fitur lanjutan dengan konteks yang konsisten

