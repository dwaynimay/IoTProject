# 📋 System Audit — IoT Health Monitor Firmware

## Arsitektur Sistem Saat Ini

```
┌─────────────────────────────────────────────────────────┐
│  SENSOR NODE (×2)                                       │
│                                                         │
│  Core 1:  taskReadIMU ─┐                               │
│  Core 1:  taskReadPPG ─┤→ g_latestImu/Ppg (shared)    │
│  Core 0:  taskCSSender ←┘  (encode + kirim via ESP-NOW) │
│  Core 0:  taskRssiExchange (kirim RSSI ke neighbor)     │
│  Core 0:  taskMonitorSensor (watchdog + stack check)    │
│  Core 0:  [DISC task, v4.x — dihapus di v5.0]          │
│                                                         │
│  Lib: EspNowMesh, CS_Model_Gaussian, DynamicRouter      │
│       HealthSensors (MPU6050, MAX30102)                 │
└───────────────────┬─────────────────────────────────────┘
                    │ ESP-NOW (7 paket/window)
                    ▼
┌─────────────────────────────────────────────────────────┐
│  GATEWAY NODE                                           │
│                                                         │
│  Core 1:  taskBeacon (broadcast 200ms/30s, lalu 2s)    │
│  Core 1:  taskMeshHandler (raw → route → mqttQueue)    │
│  Core 0:  taskMqttPublish (mqttQueue → MQTT broker)    │
│  Core 0:  taskMonitorGateway (watchdog + queue health)  │
│                                                         │
│  Lib: EspNowMesh, Network_Mqtt, MeshRouting             │
└───────────────────┬─────────────────────────────────────┘
                    │ MQTT (WiFi)
                    ▼
              [Server Python]
```

---

## 📦 Inventaris Fitur

### SENSOR NODE

| Komponen | Fungsi | Status |
|---|---|---|
| **taskReadIMU** | Baca MPU6050 @100Hz, simpan ke g_latestImu | ✅ KEEP |
| **taskReadPPG** | Baca MAX30102, simpan ke g_latestPpg | ✅ KEEP |
| **taskCSSender** | Snapshot → sanity check → CS encode → routing → kirim 7 paket | ✅ KEEP (simplify) |
| **taskRssiExchange** | Kirim RSSI self ke neighbor tiap interval | ⚠️ SIMPLIFY |
| **taskMonitorSensor** | Watchdog + stack check | ✅ KEEP |
| **CSPhiMatrix** | Singleton Hadamard-Gaussian Φ (8KB RAM, sekali generate) | ✅ KEEP |
| **CSEncoder ×7** | Encode 7 sinyal (Ax/Ay/Az/Gx/Gy/Gz/IR) | ✅ KEEP |
| **DynamicRouter** | Pilih rute: DIRECT ke gateway atau RELAY via neighbor | ⚠️ SIMPLIFY |
| **SanityLimit** | Validasi range IMU sebelum encode | ✅ KEEP |
| **FINGER_GATE** | Blokir TX jika tidak ada jari (saat ini OFF) | 🗑️ DEPRECATE |
| **BATCHING** | Batch MQTT (saat ini OFF) | 🗑️ DEPRECATE |
| **promiscuous mode** | RSSI monitoring dari beacon gateway | ⚠️ SIMPLIFY |
| **processPendingChannelSync** | Deferred channel update (v4.x) | 🗑️ DEPRECATE |
| **_taskChannelDiscovery** | Background sweep (v4.x) | 🗑️ REMOVE |

### GATEWAY NODE

| Komponen | Fungsi | Status |
|---|---|---|
| **taskBeacon** | Broadcast beacon (aggressive 200ms/30s, normal 2s) | ✅ KEEP |
| **taskMeshHandler** | Terima rawQueue → MeshRouting → mqttQueue | ✅ KEEP |
| **taskMqttPublish** | Drain mqttQueue → publish ke broker | ✅ KEEP |
| **taskMonitorGateway** | Watchdog + queue health log | ✅ KEEP |
| **MeshRouting** | Route paket CS (accumulate 7 → 1 publish) | ✅ KEEP |
| **setGatewayChannel()** | Update channel setelah WiFi connect | ✅ KEEP |

---

## 🔴 Risiko Lag / Hang yang Teridentifikasi

### Risiko Tinggi

#### 1. `taskCSSender` — 7 `esp_now_send()` per window berturutan
```
SETIAP WINDOW (setiap 10ms dari IMU_SAMPLE_MS):
  sendCsAxis(AX) → dynamic del+add peer → esp_now_send → tunggu ACK
  sendCsAxis(AY) → dynamic del+add peer → esp_now_send → tunggu ACK
  ... × 7 total
```
**Masalah**: Setiap `esp_now_send()` **blocking** sampai dapat ACK/NACK dari radio layer. 
Total blocking time per window = 7 × ~1-3ms = **7-21ms per window**.  
Dengan `IMU_SAMPLE_MS=10ms`, task ini bisa **overshoot** loop interval.

#### 2. Dynamic Peer (v4.1) — `esp_now_del_peer` + `esp_now_add_peer` per send
```
Setiap _send() ke unicast:
  esp_now_del_peer()  → internal ESP-NOW mutex lock
  esp_now_add_peer()  → internal ESP-NOW mutex lock
  esp_now_send()
```
**Masalah**: 2 mutex operation tambahan per kirim. Dengan 7 kirim per window = **14 mutex ops/window extra**.

#### 3. `vTaskDelay(pdMS_TO_TICKS(1))` di `sendCsAxis()`
```cpp
bool EspNowMesh::sendCsAxis(...) {
    ...
    vTaskDelay(pdMS_TO_TICKS(1));   // ← 1ms delay per axis!
    return ok;
}
```
Ini menambah `7 × 1ms = 7ms` delay tambahan per window, tanpa alasan yang jelas.

#### 4. `taskRssiExchange` — redundan untuk 2-node saat ini
Sistem saat ini efektif 2 node (sensor_a → gateway langsung). RSSI exchange ke neighbor dimaksudkan untuk relay, tapi dengan `MeshTopology::maxNeighborsPerNode=1` dan semua route langsung ke gateway, ini jadi overhead tanpa manfaat nyata.

#### 5. Log berlebihan di produksi
```
LOG_LEVEL = 3 (INFO)  ← setiap window log, setiap 10s log, setiap 500 stack log
```
`Serial.printf()` di ESP32 **blocking** dan bisa 0.5–2ms per baris. Dengan 5 log INFO per window × 10 Hz = **50 Serial calls/detik** yang blocking.

#### 6. `taskCSSender` delay di akhir loop = `IMU_SAMPLE_MS = 10ms`
Tapi tugas lainnya (7 send + encode) sudah makan waktu lebih. Loop period jadi tidak konsisten.

### Risiko Sedang

#### 7. `DynamicRouter` overhead
- Mutex lock di setiap `decide()` dan `updateSelfRssi()`
- `printStatus()` dipanggil setiap 5 round (50s) → Serial blocking lagi

#### 8. Stack Size mungkin terlalu besar
```
ESPNOW_TX: 12288 bytes  ← sangat besar
MQTT_PUB:   8192 bytes
SENSOR_PPG: 4096 bytes
SENSOR_IMU: 4096 bytes
```
Total stack sensor: ~25KB dari ~320KB heap. Bukan masalah kritis, tapi perlu audit.

#### 9. `processPendingChannelSync()` dipanggil setiap window
Di v5.0, `s_channelConfirmed` sudah true sejak `begin()`. Fungsi ini selalu return false, tapi tetap dipanggil setiap loop di `taskCSSender` — overhead kecil tapi unnecessary.

---

## ✅ Rekomendasi: Lean Architecture

### Prioritas 1 — HAPUS Segera (Zero Risk)

| Yang Dihapus | Alasan | Dampak |
|---|---|---|
| `vTaskDelay(1ms)` di `sendCsAxis()` | Tidak ada alasan teknis, hanya menambah latency | -7ms/window |
| `processPendingChannelSync()` di loop CSSender | v5.0: selalu no-op | Kode lebih bersih |
| `_taskChannelDiscovery()` body | Sudah diganti v5.0, tinggal stub | RAM task stack -3KB |
| `FINGER_GATE` (sudah OFF) | Feature flag mati, dead code | Kode lebih bersih |
| `BATCHING` (sudah OFF) | Feature flag mati, dead code | Kode lebih bersih |

### Prioritas 2 — SEDERHANAKAN (Medium Risk, High Reward)

| Yang Disederhanakan | Dari | Ke |
|---|---|---|
| **Dynamic peer** di `_send()` | del+add per kirim (7×/window) | Hanya update channel jika berubah (`esp_now_mod_peer` sekali) |
| **Log level** di produksi | LOG_LEVEL=3 (INFO, banjir) | LOG_LEVEL=2 (WARN only) |
| **taskRssiExchange** | Kirim RSSI ke neighbor tiap interval | Nonaktifkan jika topologi 2-node |
| **DynamicRouter** | Full routing engine dengan mutex | Untuk 2-node: hardcode DIRECT, hapus router |

### Prioritas 3 — PERTAHANKAN & OPTIMALKAN

| Yang Dipertahankan | Catatan Optimasi |
|---|---|
| **CS Encoding (7 sinyal)** | Sudah optimal: singleton Φ, O(M×N) per encode |
| **SanityLimit IMU check** | Penting untuk mencegah corrupt data |
| **taskBeacon gateway** | Aggressive 30s sudah bagus |
| **MeshRouting gateway** | Accumulate 7→1 sudah efisien |
| **Watchdog** | Penting untuk long-running |
| **WiFi channel sync v5.0** | Baru, sudah proven |

---

## 🎯 Target Arsitektur Lean (untuk TA)

### Sensor Node — 3 Task saja (dari 5)

```
Core 1:  taskReadIMU  (100Hz, baca MPU6050)
Core 1:  taskReadPPG  (polling MAX30102)
Core 0:  taskCSSender (encode + send, TANPA blocking log)
Core 0:  taskMonitor  (watchdog, jarang jalan)
```

**Hilangkan**: `taskRssiExchange` (tidak relevan jika 2-node)

### Gateway Node — Tetap 4 Task

```
Core 1:  taskBeacon        (beacon periodik)
Core 1:  taskMeshHandler   (raw → MQTT queue)
Core 0:  taskMqttPublish   (queue → broker)
Core 0:  taskMonitor       (watchdog)
```

### Perubahan Kode Kritis

```diff
// 1. Hapus delay 1ms di sendCsAxis()
- vTaskDelay(pdMS_TO_TICKS(1));

// 2. Hapus processPendingChannelSync() dari loop
- g_mesh.processPendingChannelSync();

// 3. Kurangi log level produksi
- #define LOG_LEVEL 3
+ #define LOG_LEVEL 2

// 4. Untuk 2-node: skip RSSI exchange task
//    atau set RSSI_EXCHANGE_MS = 60000 (1 menit)

// 5. Dynamic peer: jangan del+add setiap send
//    Cukup esp_now_mod_peer jika channel berubah
```

---

## 📊 Estimasi Efek Setelah Optimasi

| Metric | Sebelum | Setelah |
|---|---|---|
| **Blocking time/window** | ~21ms (7 send + 7ms delay) | ~7ms (7 send) |
| **Serial calls/detik** | ~50 | ~5 |
| **Active tasks sensor** | 5 | 4 |
| **Extra mutex ops/window** | 14 (dynamic peer) | 0 |
| **RAM freed** | — | ~3KB (DISC stack) |

---

## ❓ Open Questions untuk Keputusan

> [!IMPORTANT]
> **Q1: Apakah multi-hop relay (sensor → sensor → gateway) masih dibutuhkan untuk TA?**  
> Jika TIDAK → hapus DynamicRouter + taskRssiExchange + RoutedCsPacket → sistem jauh lebih simpel.  
> Jika YA → pertahankan, tapi nonaktifkan relay selama node dalam jangkauan langsung.

> [!IMPORTANT]  
> **Q2: Apakah PPG (SpO2 + HR) wajib dikirim real-time?**  
> PPG sangat bergantung jari menempel. Kalau tidak ada jari, encoder kirim nol terus.  
> Bisa dipertimbangkan: kirim PPG hanya jika `finger=true`, jika tidak skip packet IR.

> [!NOTE]
> **Q3: CS encoding window N=64 dengan 7 sinyal → 7 paket per window.**  
> Dengan `SEND_INTERVAL=200ms`, window 64 sampel @10ms = 640ms per encode.  
> Artinya sensor encode+kirim sekali setiap ~640ms, bukan 200ms.  
> Apakah ini sesuai ekspektasi throughput di TA?
