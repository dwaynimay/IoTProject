# Pengujian End-to-End Otomatis

Dokumen ini menjelaskan mode uji end-to-end otomatis yang memakai data dummy
terstruktur, manipulasi RSSI, ESP-NOW mesh, dan publish MQTT di gateway.

## Tujuan

Pengujian ini membuktikan rantai penuh:

1. node 1 membangkitkan `cs_imu`
2. node 2 membangkitkan `cs_ppg`
3. node 1 dapat memilih jalur `direct` atau `relay` berdasarkan RSSI scripted
4. node 2 dapat me-relay `cs_imu` saat dibutuhkan
5. gateway menerima dan mengubah paket menjadi payload MQTT
6. gateway publish ke broker pada topic sistem yang sama dengan firmware utama

## Firmware

- Gateway: `test_e2e_gateway`
- Node 1 IMU sender: `test_e2e_sensor_n1`
- Node 2 PPG sender + relay: `test_e2e_sensor_n2`

## Output yang Dihasilkan

### Serial

- `[TX_IMU]` dari node 1
- `[TX_PPG]` dari node 2
- `[RELAY]` dari node 2
- `[MQTT]` dari gateway

### Broker MQTT

Script capture subscribe ke:

- `health_monitor/+/cs_imu`
- `health_monitor/+/cs_ppg`

## Script Capture

Gunakan:

```powershell
D:\Github\perbaikan\IoTProject\server\.venv\Scripts\python.exe -m server.tools.capture_end_to_end_test --gateway-port COM7 --imu-port COM15 --ppg-port COM3 --duration 430
```

## File Hasil

- `summary_tx_imu.csv`
- `summary_tx_ppg.csv`
- `summary_gateway_mqtt_publish.csv`
- `summary_broker_receive.csv`
- `report.json`
- `README_HASIL_END_TO_END.txt`

## Makna Pembuktian

Jika:

- node 1 mengirim `cs_imu`
- node 2 mengirim `cs_ppg`
- gateway mencetak `[MQTT] ok=1`
- broker menerima topik `cs_imu` dan `cs_ppg`

maka sistem dapat dinyatakan berhasil melakukan pengiriman data end-to-end dari
node mesh hingga broker MQTT dengan skenario direct dan relay yang disimulasikan
oleh manipulasi RSSI.
