# Pengujian Compressive Sensing untuk TA

Dokumen ini merangkum alur pengujian IMU, PPG, dan komunikasi agar hasilnya rapi dan mudah dimasukkan ke buku TA.

## 1. Tujuan Pengujian

Pengujian dibagi menjadi tiga kelompok:

1. Pengujian akurasi compressive sensing pada sinyal IMU.
2. Pengujian akurasi compressive sensing pada sinyal PPG.
3. Pengujian komunikasi sistem secara nirkabel.

## 2. Ringkasan Media Uji

| Jenis uji | Sensor | Media | Tujuan |
|---|---|---|---|
| CS IMU | MPU6050 | USB serial | Bandingkan raw vs rekonstruksi |
| CS PPG | MAX30102 | USB serial | Bandingkan raw vs rekonstruksi |
| Komunikasi | IMU + PPG + gateway | Wireless ESP-NOW/WiFi/MQTT | Uji pengiriman data sistem |

## 3. Pengujian IMU

### Hardware

- 1 ESP32
- 1 MPU6050
- Kabel USB ke laptop

### Firmware

- Environment: `test_imu_raw_vs_cs`
- File sketch: [test_imu_raw_vs_cs.cpp](D:/Github/perbaikan/IoTProject/firmware/test_sketches/test_imu_raw_vs_cs.cpp)

### Perintah upload

```powershell
cd D:\Github\perbaikan\IoTProject\firmware
pio run -e test_imu_raw_vs_cs -t upload
```

### Perintah rekam data

```powershell
cd D:\Github\perbaikan\IoTProject
.\server\.venv\Scripts\python.exe -m server.tools.capture_cs_test --port COM5 --windows 10
```

### Output untuk TA

- `summary_axis_mean.csv`
- `summary_metrics.csv`
- `plot_overlay_all_windows.png`
- `plot_metric_trends.png`

### Metrik yang dilaporkan

- RMSE
- MAE
- MAPE
- SNR
- Korelasi

## 4. Pengujian PPG

### Hardware

- 1 ESP32
- 1 MAX30102
- Kabel USB ke laptop

### Firmware

- Environment: `test_ppg_raw_vs_cs`
- File sketch: [test_ppg_raw_vs_cs.cpp](D:/Github/perbaikan/IoTProject/firmware/test_sketches/test_ppg_raw_vs_cs.cpp)

### Perintah upload

```powershell
cd D:\Github\perbaikan\IoTProject\firmware
pio run -e test_ppg_raw_vs_cs -t upload
```

### Perintah rekam data

```powershell
cd D:\Github\perbaikan\IoTProject
.\server\.venv\Scripts\python.exe -m server.tools.capture_cs_test_ppg --port COM5 --windows 10
```

### Output untuk TA

- `summary_metrics_ppg.csv`
- `plot_overlay_ppg.png`
- `plot_metric_trends_ppg.png`

### Metrik yang dilaporkan

- RMSE
- MAE
- SNR
- Korelasi
- `ppg_valid`
- Heart rate
- SpO2

## 5. Pengujian Komunikasi Wireless

Pengujian ini berbeda dari pengujian CS isolasi. Pada tahap ini data diuji melalui jalur komunikasi sebenarnya.

### Tujuan

- Memastikan data bisa dikirim dari node sensor ke gateway.
- Memastikan gateway bisa meneruskan data ke MQTT/server.
- Memastikan data berhasil direkonstruksi dan disimpan.

### Kebutuhan perangkat

- Node IMU
- Node PPG
- Node gateway
- Broker MQTT
- WiFi yang sama untuk sinkronisasi channel

### Environment yang dipakai

- `node_imu`
- `node_ppg`
- `node_gateway`

### Contoh hal yang diuji

- Persentase paket terkirim
- Delay pengiriman
- Stabilitas koneksi
- Kesesuaian data yang diterima server

## 6. Susunan Subbab Pengujian TA

Struktur yang disarankan:

1. Lingkungan dan perangkat pengujian
2. Pengujian compressive sensing pada sinyal IMU
3. Pengujian compressive sensing pada sinyal PPG
4. Pengujian komunikasi nirkabel
5. Pengujian sistem end-to-end
6. Analisis hasil pengujian

## 7. Template Narasi Singkat

### Pengujian IMU

Pengujian IMU dilakukan untuk mengevaluasi kemampuan metode compressive sensing dalam merekonstruksi sinyal akselerometer dan giroskop setelah proses kompresi. Pengujian dilakukan menggunakan satu node ESP32 yang terhubung ke sensor MPU6050. Data mentah dan data hasil kompresi dikirim ke komputer melalui komunikasi serial USB, kemudian direkonstruksi menggunakan algoritma OMP pada sisi server.

### Pengujian PPG

Pengujian PPG dilakukan untuk mengevaluasi kemampuan compressive sensing dalam mempertahankan bentuk sinyal PPG setelah proses kompresi dan rekonstruksi. Pengujian menggunakan satu node ESP32 yang terhubung ke sensor MAX30102. Data hasil pengukuran dikirim melalui serial USB agar analisis dapat difokuskan pada kualitas algoritma, tanpa dipengaruhi faktor komunikasi nirkabel.

### Pengujian komunikasi

Pengujian komunikasi dilakukan untuk memastikan sistem mampu mengirimkan data sensor dari node ke gateway dan selanjutnya ke server melalui jaringan yang dirancang. Pada tahap ini komunikasi tidak lagi menggunakan kabel USB sebagai media utama pengambilan data, melainkan menggunakan ESP-NOW, WiFi, dan MQTT sesuai arsitektur sistem.
