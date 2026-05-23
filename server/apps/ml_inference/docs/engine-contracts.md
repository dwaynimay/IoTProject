# Engine Contracts — Referensi Lengkap

Dokumen ini adalah sumber kebenaran tentang apa yang didukung engine. Baca ini saat ada pertanyaan tentang fitur, sinyal, atau config yang valid.

---

## Tipe Fitur yang Didukung

### `"stat"` — Statistik dari sinyal

```json
{"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"}
```

| `stat` | Keterangan | Implementasi Python |
|--------|-----------|-------------------|
| `mean` | Rata-rata | `np.mean(a)` |
| `std` | Standar deviasi | `np.std(a)` |
| `min` | Nilai minimum | `np.min(a)` |
| `max` | Nilai maksimum | `np.max(a)` |
| `rms` | Root Mean Square | `np.sqrt(np.mean(a**2))` |
| `energy` | Energi sinyal | `np.sum(a**2)` |
| `range` | Max - Min | `np.max(a) - np.min(a)` |
| `zcr` | Zero Crossing Rate | `np.sum(np.diff(np.sign(a)) != 0) / len(a)` |
| `median` | Median | `np.median(a)` |
| `p25` / `q1` | Persentil 25 | `np.percentile(a, 25)` |
| `p75` / `q3` | Persentil 75 | `np.percentile(a, 75)` |
| `skew` | Skewness | `scipy.stats.skew(a)` |
| `kurt` | Kurtosis (Fisher/excess) | `scipy.stats.kurtosis(a)` — Fisher, bukan Pearson |
| `peak_freq` | Frekuensi dominan | `freqs[argmax(abs(rfft(a))[1:])+1]` |
| `spectral_energy` | Energi spektral | `sum(abs(rfft(a))**2) / len(a)` |

> ⚠ **`kurt` di engine = Fisher (excess kurtosis)** → untuk distribusi normal = 0.
> `scipy.stats.kurtosis(a)` secara default juga Fisher — konsisten.
> Jangan pakai `scipy.stats.kurtosis(a, fisher=False)` karena itu Pearson (normal = 3).

### `"derived"` — Sinyal computed

```json
{"name": "smv_mean", "type": "derived", "formula": "smv", "stat": "mean"}
```

| `formula` | Rumus | Sinyal yang dibutuhkan |
|-----------|-------|----------------------|
| `smv` | `sqrt(ax² + ay² + az²)` | ax, ay, az |
| `smv_gyro` | `sqrt(gx² + gy² + gz²)` | gx, gy, gz |

`stat` yang tersedia: sama dengan tabel `"stat"` di atas.

### `"cross"` — Fitur antar sinyal

```json
{"name": "corr_xy", "type": "cross", "signal_a": "ax", "signal_b": "ay", "cross_type": "corr", "default": 0.0}
```

| `cross_type` | Keterangan | Implementasi Python |
|--------------|-----------|-------------------|
| `corr` | Korelasi Pearson | `np.corrcoef(a, b)[0,1]` |
| `dot` | Dot product ternormalisasi | `np.dot(a,b) / (len(a))` |
| `diff_energy` | Energi selisih | `np.sum((a-b)**2)` |

Field `"default"`: nilai jika sinyal tidak tersedia (None). Wajib ada untuk `cross`.

### `"meta"` — Metadata sensor

```json
{"name": "hr",     "type": "meta", "field": "hr",     "default": -1}
{"name": "finger", "type": "meta", "field": "finger",  "cast": "int", "default": 0}
```

| `field` | Tipe | Keterangan |
|---------|------|-----------|
| `hr` | int | Heart rate dari firmware |
| `spo2` | float | Saturasi oksigen |
| `finger` | bool → int | Status jari terdeteksi (cast: "int" → 0/1) |
| `ts` | float | Timestamp window |
| `node_id` | int | ID node sensor |
| `window_num` | int | Nomor window dalam sesi |

**Catatan `cast`:** `"int"` mengkonversi bool → 0/1 integer. Tanpa `cast`, default float.

### `"raw"` — Elemen sinyal by index

```json
{"name": "ax_raw_0", "type": "raw", "signal": "ax", "index": 0}
```

Ambil satu nilai dari posisi tertentu dalam window. Jarang dipakai — hanya jika model butuh raw sample.

---

## Sinyal Input yang Tersedia di Engine

| Sinyal | Jenis | Tipe data | Panjang |
|--------|-------|-----------|---------|
| `ax` | IMU Accel X | list[float] | CS_N = 64 |
| `ay` | IMU Accel Y | list[float] | CS_N = 64 |
| `az` | IMU Accel Z | list[float] | CS_N = 64 |
| `gx` | IMU Gyro X | list[float] | CS_N = 64 |
| `gy` | IMU Gyro Y | list[float] | CS_N = 64 |
| `gz` | IMU Gyro Z | list[float] | CS_N = 64 |
| `ir` | PPG Infrared | list[float] | CS_N = 64 |
| `hr` | Heart rate | int | scalar |
| `spo2` | SpO2 | float | scalar |
| `finger` | Finger detected | bool | scalar |

Sinyal yang tidak tersedia = `None` → fitur fallback ke nilai `"default"`.

---

## Kontrak `skip_if`

Engine skip window ini dan tidak memanggil model jika kondisi terpenuhi.

```json
"skip_if": {
    "finger_required": true,          // skip jika finger=False/None
    "min_hr": 20,                     // skip jika hr < 20
    "max_hr": 250,                    // skip jika hr > 250
    "require_signals": ["ax","ay","az"] // skip jika salah satu sinyal None
}
```

Semua field opsional. Jika `skip_if` tidak ada, engine tidak skip.

---

## Kontrak `output`

```json
"output": {
    "confidence_threshold": 0.30
}
```

Jika `max(proba) < confidence_threshold` → engine return label `"uncertain"`.
Set ke `0.0` untuk disable threshold (selalu return label).

---

## Folder Struktur Engine

```
server/apps/ml_inference/models/
├── imu/      ← Model berbasis IMU (ax/ay/az/gx/gy/gz)
├── ppg/      ← Model berbasis PPG (ir/hr/spo2)
└── fusion/   ← Model multi-modal (IMU + PPG)
```

Setiap model butuh **dua file di folder yang sama**:
- `<name>.pkl` — sklearn Pipeline
- `<name>_config.json` — konfigurasi fitur

---

## Validasi yang Dilakukan Engine

1. Load `<name>.pkl` → cek `hasattr(model, 'predict_proba')`
2. Jika ada `n_features_in_` → validasi `n_features_in_ == len(config["features"])`
3. Urutan `config["labels"]` dipakai langsung sebagai output label — harus sama dengan `model.classes_`
4. Tiap window → `FeatureExtractor` build vector → `model.predict_proba(X)` → ambil argmax

---

## Contoh Config Lengkap — IMU

```json
{
    "model_name"    : "fall_detection_svm_v1",
    "model_version" : "1.0.0",
    "description"   : "Fall detection 4-class — SVM RBF, UMA ADL FALL dataset",
    "author"        : "imu-team",
    "trained_at"    : "2025-05",
    "labels"        : ["duduk", "jalan", "jatuh", "tidur"],
    "features"      : [
        {"name": "ax_mean",  "type": "stat",    "signal": "ax", "stat": "mean"},
        {"name": "ax_std",   "type": "stat",    "signal": "ax", "stat": "std"},
        {"name": "smv_mean", "type": "derived", "formula": "smv", "stat": "mean"},
        {"name": "corr_xy",  "type": "cross",   "signal_a": "ax", "signal_b": "ay",
         "cross_type": "corr", "default": 0.0}
    ],
    "skip_if"       : {"require_signals": ["ax", "ay", "az"]},
    "output"        : {"confidence_threshold": 0.30}
}
```

## Contoh Config Lengkap — PPG

```json
{
    "model_name" : "hr_classifier_v1",
    "labels"     : ["bradycardia", "normal", "tachycardia"],
    "features"   : [
        {"name": "ir_mean",           "type": "stat", "signal": "ir", "stat": "mean",           "default": 0.0},
        {"name": "ir_std",            "type": "stat", "signal": "ir", "stat": "std",            "default": 0.0},
        {"name": "ir_peak_freq",      "type": "stat", "signal": "ir", "stat": "peak_freq",      "default": 0.0},
        {"name": "ir_spectral_energy","type": "stat", "signal": "ir", "stat": "spectral_energy","default": 0.0},
        {"name": "hr",                "type": "meta", "field": "hr",   "default": -1},
        {"name": "spo2",              "type": "meta", "field": "spo2", "default": 0.0}
    ],
    "skip_if"  : {"finger_required": true, "min_hr": 20, "max_hr": 250},
    "output"   : {"confidence_threshold": 0.40}
}
```
