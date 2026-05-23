# Panduan Membuat & Mengintegrasikan Model ML

> Dokumentasi ini menjelaskan kontrak, syarat, dan cara membuat model `.pkl` beserta `_config.json` agar dapat digunakan oleh **MLInferenceEngine** dan **ModelRegistry**.

---

## Daftar Isi

1. [Arsitektur Singkat](#arsitektur-singkat)
2. [Kontrak Model — Syarat Wajib](#kontrak-model--syarat-wajib)
3. [Kontrak Config JSON](#kontrak-config-json)
4. [Tipe Fitur yang Didukung](#tipe-fitur-yang-didukung)
5. [Sinyal Input yang Tersedia](#sinyal-input-yang-tersedia)
6. [Syarat Window Size](#syarat-window-size)
7. [Menggunakan Scaler / Preprocessing](#menggunakan-scaler--preprocessing)
8. [Model Disimpan sebagai Dict (Non-Standar)](#model-disimpan-sebagai-dict-non-standar)
9. [Struktur Folder Models](#struktur-folder-models)
10. [Cara Menambah Model Baru](#cara-menambah-model-baru)
11. [Checklist sebelum Deploy](#checklist-sebelum-deploy)
12. [Contoh Lengkap](#contoh-lengkap)

---

## Arsitektur Singkat

```
WindowInput (sensor data)
    │
    ▼
FeatureExtractor  ←── feature schema dari _config.json
    │                  (stat / meta / raw / cross / derived)
    ▼
ndarray (1 × n_features, float64)
    │
    ▼
model.predict_proba(X)   ←── file .pkl
    │
    ▼
InferenceResult { label, confidence, proba }
```

Engine tidak tahu dan tidak peduli bagaimana model di-train. Yang penting: **interface kontrak terpenuhi**.

---

## Kontrak Model — Syarat Wajib

### 1. File `.pkl` harus expose `predict_proba()`

```python
proba = model.predict_proba(X)
# X     : ndarray shape (n_samples, n_features), dtype float64
# proba : ndarray shape (n_samples, n_classes),  dtype float64
# proba.sum(axis=1) ≈ 1.0 untuk setiap baris
```

Model yang memenuhi syarat ini:
- `sklearn` estimator dengan `probability=True` (SVC, RandomForest, GradientBoosting, dll)
- `sklearn` Pipeline di mana step terakhir punya `predict_proba()`
- Wrapper custom yang implement interface di atas (lihat [Wrapper](#model-disimpan-sebagai-dict-non-standar))

> [!IMPORTANT]
> Model **harus minimal 2 kelas**. Engine memanggil `max()` pada probabilitas.
> Model regresi (output single float) tidak didukung secara native — perlu wrapper.

### 2. `n_features_in_` disarankan ada

Jika model punya atribut `n_features_in_`, engine akan otomatis validasi jumlah fitur config vs model:

```python
if hasattr(model, "n_features_in_"):
    assert model.n_features_in_ == len(config["features"])
```

Jika tidak ada, validasi dilewati (aman, tapi tidak ada proteksi mismatch).

### 3. Urutan label harus konsisten

Urutan kelas di `predict_proba()` output **harus sama** dengan urutan `"labels"` di config JSON.

```python
# Contoh: model punya classes_ = ["duduk", "jalan", "jatuh", "tidur"]
# Config harus: "labels": ["duduk", "jalan", "jatuh", "tidur"]  ← urutan SAMA
```

Untuk sklearn estimator: `model.classes_` menunjukkan urutan aktual.

---

## Kontrak Config JSON

File `<name>_config.json` harus berada di folder yang sama dengan `.pkl`.

### Struktur Minimal

```json
{
    "model_name": "nama_model_v1",
    "model_version": "1.0.0",
    "labels": ["kelas_a", "kelas_b"],
    "features": [
        {"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"}
    ]
}
```

### Struktur Lengkap

```json
{
    "model_name"    : "activity_classifier_v1",
    "model_version" : "1.0.0",
    "description"   : "Deskripsi singkat model",
    "author"        : "nama-tim",
    "trained_at"    : "2025-05",

    "labels": ["duduk", "jalan", "jatuh", "tidur"],

    "features": [
        {"name": "ax_mean",  "type": "stat",    "signal": "ax", "stat": "mean"},
        {"name": "smv_mean", "type": "derived",  "formula": "smv", "stat": "mean"},
        {"name": "corr_xy",  "type": "cross",    "signal_a": "ax", "signal_b": "ay", "cross_type": "corr", "default": 0.0},
        {"name": "hr",       "type": "meta",     "field": "hr",    "default": -1},
        {"name": "finger",   "type": "meta",     "field": "finger","cast": "int"}
    ],

    "skip_if": {
        "finger_required"  : false,
        "min_hr"           : 20,
        "max_hr"           : 250,
        "require_signals"  : ["ax", "ay", "az"]
    },

    "output": {
        "confidence_threshold": 0.0
    }
}
```

| Field | Wajib | Keterangan |
|-------|-------|-----------|
| `labels` | ✅ | List kelas, minimal 2, urutan = urutan `predict_proba()` |
| `features` | ✅ | Schema fitur, urutan harus sama persis dengan training |
| `model_name` | — | Dipakai untuk logging & status API |
| `model_version` | — | Versi untuk tracking |
| `skip_if` | — | Kondisi untuk skip window |
| `output.confidence_threshold` | — | Di bawah threshold → label = "uncertain" |

---

## Tipe Fitur yang Didukung

### `"stat"` — Statistik dari sinyal rekonstruksi

```json
{"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"}
```

`stat` yang tersedia: `mean`, `std`, `min`, `max`, `rms`, `energy`, `range`, `zcr`, `median`, `p25`/`q1`, `p75`/`q3`, `skew`, `kurt`, `peak_freq`, `spectral_energy`

### `"derived"` — Sinyal computed dari kombinasi sinyal lain

```json
{"name": "smv_mean", "type": "derived", "formula": "smv", "stat": "mean"}
```

`formula` yang tersedia:
- `"smv"` → `sqrt(ax² + ay² + az²)` — Signal Magnitude Vector (IMU accelerometer)
- `"smv_gyro"` → `sqrt(gx² + gy² + gz²)` — SMV gyroscope

### `"cross"` — Fitur cross-signal

```json
{"name": "corr_xy", "type": "cross", "signal_a": "ax", "signal_b": "ay", "cross_type": "corr", "default": 0.0}
```

`cross_type` yang tersedia: `corr` (Pearson), `dot`, `diff_energy`

### `"meta"` — Metadata sensor

```json
{"name": "hr",     "type": "meta", "field": "hr",     "default": -1},
{"name": "spo2",   "type": "meta", "field": "spo2",   "default": 0.0},
{"name": "finger", "type": "meta", "field": "finger",  "cast": "int", "default": 0}
```

`field` yang tersedia: `hr`, `spo2`, `finger`, `ts`, `node_id`, `window_num`

`cast`: `"int"` (konversi bool→int 0/1), `"float"` (default)

### `"raw"` — Satu elemen dari sinyal (by index)

```json
{"name": "ax_raw_0", "type": "raw", "signal": "ax", "index": 0}
```

### Field `"default"`

Jika sinyal tidak tersedia (None), fitur menggunakan nilai `default` (default: `0.0`). Fitur yang fallback ke default dicatat di log untuk QA.

---

## Sinyal Input yang Tersedia

`WindowInput` menyediakan sinyal berikut (semua `list[float]` panjang `CS_N`, atau `None` jika tidak tersedia):

| Sinyal | Jenis | Keterangan |
|--------|-------|-----------|
| `ax` | IMU | Accelerometer X — rekonstruksi CS |
| `ay` | IMU | Accelerometer Y — rekonstruksi CS |
| `az` | IMU | Accelerometer Z — rekonstruksi CS |
| `gx` | IMU | Gyroscope X — rekonstruksi CS |
| `gy` | IMU | Gyroscope Y — rekonstruksi CS |
| `gz` | IMU | Gyroscope Z — rekonstruksi CS |
| `ir` | PPG | Sinyal infrared — rekonstruksi CS |
| `hr` | Meta | Heart rate (int, dari firmware) |
| `spo2` | Meta | Saturasi oksigen (float, dari firmware) |
| `finger` | Meta | Status deteksi jari (bool) |

---

## Syarat Window Size

> [!IMPORTANT]
> **CS rekonstruksi menghasilkan sinyal panjang 64 sample** (`CS_N = 64`).
> Model yang di-train dengan `window_size=64` akan menghasilkan fitur statistik yang konsisten.

### Aturan:

| Kondisi | Status |
|---------|--------|
| Model dilatih dengan `window_size=64` | ✅ Direkomendasikan |
| Model dilatih dengan `window_size` lain (misal 100) | ⚠️ Fitur statistik (mean/std/rms/range) masih relatif stabil. Fitur frekuensi (peak_freq, spectral_energy) terpengaruh resolusi. Monitor hasil di produksi. |
| Model dilatih dengan sinyal raw (bukan fitur statistik) | ❌ Tidak cocok — panjang array berbeda. Gunakan fitur statistik saja. |

### Kenapa 64?

Pipeline CS (Compressive Sensing) di server ini merekonstruksi `CS_N=64` sample per window dari measurement yang lebih sedikit. Angka ini konsisten di seluruh pipeline dan tidak dapat dikonfigurasi per-model.

---

## Menggunakan Scaler / Preprocessing

### Opsi A: Scaler di dalam sklearn Pipeline (Direkomendasikan)

Jika scaler disimpan di dalam sklearn `Pipeline`, **tidak perlu file terpisah**. Engine langsung panggil `pipeline.predict_proba(X)` — sklearn Pipeline otomatis apply semua transform.

```python
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC

model = Pipeline([
    ("scaler", StandardScaler()),
    ("clf",    SVC(probability=True)),
])
model.fit(X_train, y_train)

# Simpan pipeline langsung — scaler sudah di dalamnya
import pickle
pickle.dump(model, open("model.pkl", "wb"), protocol=4)
```

**Ini cara terbaik** — tidak ada file tambahan, tidak ada masalah urutan load.

### Opsi B: Scaler sebagai file `.pkl` terpisah

Jika scaler disimpan terpisah (misal `model_scaler.pkl`), gunakan wrapper:

```python
# wrappers.py — tambah class baru
class ModelWithSeparateScaler:
    def __init__(self, scaler, model, classes):
        self._scaler      = scaler
        self._model       = model
        self.classes_     = np.array(classes)
        self.n_features_in_ = scaler.n_features_in_

    def predict_proba(self, X):
        return self._model.predict_proba(self._scaler.transform(X))
```

Buat wrapper pkl sekali:
```python
from apps.ml_inference.wrappers import ModelWithSeparateScaler
import pickle

scaler = pickle.load(open("model_scaler.pkl", "rb"))
model  = pickle.load(open("model_raw.pkl", "rb"))

wrapper = ModelWithSeparateScaler(scaler, model, classes=["a", "b"])
pickle.dump(wrapper, open("model_wrapper.pkl", "wb"), protocol=4)
```

Setelah dibuat wrapper, **hanya wrapper.pkl yang dipakai engine** — scaler sudah di dalamnya.

> [!NOTE]
> Engine hanya membaca **satu file `.pkl` per model**. Jika ada file pendukung (scaler, encoder, dll), semuanya harus di-bundle ke dalam wrapper sebelum disimpan sebagai pkl.

### Opsi C: Preprocessing custom (non-sklearn)

Implementasikan `predict_proba()` di wrapper dan lakukan preprocessing di dalamnya:

```python
class CustomPreprocessWrapper:
    def __init__(self, model, mean, std):
        self._model = model
        self._mean  = np.array(mean)
        self._std   = np.array(std)
        self.classes_ = np.array(["a", "b", "c"])
        self.n_features_in_ = len(mean)

    def predict_proba(self, X):
        X_norm = (X - self._mean) / (self._std + 1e-9)
        return self._model.predict_proba(X_norm)
```

---

## Model Disimpan sebagai Dict (Non-Standar)

Beberapa training script menyimpan model sebagai `dict` (bukan estimator langsung). Ini perlu dibungkus wrapper.

### Deteksi masalah ini:
```python
import pickle
m = pickle.load(open("model.pkl", "rb"))
print(type(m))           # dict? bukan sklearn estimator?
print(hasattr(m, "predict_proba"))  # False? → perlu wrapper
```

### Solusi: Buat wrapper class di `wrappers.py`

> [!CAUTION]
> **Wrapper class WAJIB didefinisikan di `apps/ml_inference/wrappers.py`**, bukan di script sementara atau `__main__`. Pickle menyimpan module path class — jika class ada di `__main__`, pickle tidak bisa di-load kembali dari konteks lain.

```python
# Tambah di server/apps/ml_inference/wrappers.py
class NamaBaru:
    def __init__(self, ...): ...
    def predict_proba(self, X): ...
    # Wajib: self.classes_, self.n_features_in_
```

Buat wrapper pkl dengan script sekali-jalan (`make_<name>_wrapper.py`):
```python
# Jalankan dari root server/:
#   .venv\Scripts\python make_model_wrapper.py
from apps.ml_inference.wrappers import NamaBaru
...
pickle.dump(wrapper, open("models/imu/nama_wrapper.pkl", "wb"), protocol=4)
```

---

## Struktur Folder Models

```
server/apps/ml_inference/models/
├── imu/                          ← Model berbasis IMU (accelerometer / gyroscope)
│   ├── fall_detection_svm_wrapper.pkl
│   ├── fall_detection_svm_wrapper_config.json
│   └── fall_detection_svm.pkl   ← pkl asli (referensi, tidak di-load engine)
├── ppg/                          ← Model berbasis PPG (sinyal IR, HR, SpO2)
│   └── README.md
└── README.md
```

Tambah kategori baru sesuai kebutuhan (misal `fusion/` untuk model multi-modal).

---

## Cara Menambah Model Baru

### 1. Pastikan model sudah sklearn-compatible

```python
import pickle
m = pickle.load(open("model.pkl", "rb"))
assert hasattr(m, "predict_proba"), "Perlu wrapper!"
```

### 2. Jika perlu wrapper, tambah class di `wrappers.py` lalu generate

```python
from apps.ml_inference.wrappers import KelasBaru
wrapper = KelasBaru(...)
pickle.dump(wrapper, open("models/imu/nama_wrapper.pkl", "wb"), protocol=4)
```

### 3. Buat file `_config.json`

Letakkan di folder yang sama dengan `.pkl`. Pastikan:
- `"labels"` urutannya sama dengan `model.classes_`
- `"features"` urutannya **persis sama** dengan urutan fitur saat training
- `n_features_in_` model == `len(config["features"])`

### 4. Verifikasi

```python
from apps.ml_inference import ModelRegistry
r = ModelRegistry()
r.scan("apps/ml_inference/models/", recursive=True)
print(r.status())
# Pastikan: loaded=True, n_features sesuai, labels benar
```

### 5. Tidak perlu restart untuk model baru

Hot-register tanpa restart server:
```python
registry.register("models/imu/model_baru.pkl", "models/imu/model_baru_config.json")
# atau scan ulang:
registry.scan("models/", recursive=True)
```

---

## Checklist sebelum Deploy

```
[ ] .pkl expose predict_proba(X) → ndarray (n_samples, n_classes)
[ ] .pkl ada di subfolder yang sesuai (imu/ atau ppg/)
[ ] _config.json ada di folder yang SAMA dengan .pkl
[ ] "labels" di config = urutan kelas dari model.classes_
[ ] "features" di config = urutan fitur saat training (tidak boleh berubah!)
[ ] Jumlah fitur config == n_features_in_ model
[ ] skip_if dikonfigurasi sesuai sinyal yang dibutuhkan
[ ] Jika model non-standar (dict / scaler terpisah): wrapper sudah di wrappers.py
[ ] smoke test: registry.scan() → loaded=True, predict() tidak SKIP
```

---

## Contoh Lengkap

### Contoh 1 — RandomForest sklearn Pipeline (Paling Sederhana)

**Training:**
```python
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import RandomForestClassifier
import pickle

model = Pipeline([
    ("scaler", StandardScaler()),
    ("clf",    RandomForestClassifier(n_estimators=100, random_state=42)),
])
model.fit(X_train, y_train)

print("classes_:", model.classes_)          # ['duduk', 'jalan', 'jatuh', 'tidur']
print("n_features_in_:", model.n_features_in_)  # 44

pickle.dump(model, open("models/imu/rf_activity.pkl", "wb"), protocol=4)
```

**Config (`models/imu/rf_activity_config.json`):**
```json
{
    "model_name": "rf_activity_v1",
    "model_version": "1.0.0",
    "labels": ["duduk", "jalan", "jatuh", "tidur"],
    "features": [
        {"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"},
        ...
    ],
    "skip_if": {"require_signals": ["ax", "ay", "az"]},
    "output": {"confidence_threshold": 0.3}
}
```

### Contoh 2 — Model dengan Scaler Terpisah

```python
# Di wrappers.py (tambah class ini):
class RFWithExternalScaler:
    def __init__(self, scaler, rf_model, classes):
        self._scaler       = scaler
        self._model        = rf_model
        self.classes_      = np.array(classes)
        self.n_features_in_ = len(classes)  # atau scaler.n_features_in_

    def predict_proba(self, X):
        return self._model.predict_proba(self._scaler.transform(X))

# Script make_rf_wrapper.py:
from apps.ml_inference.wrappers import RFWithExternalScaler
scaler = pickle.load(open("rf_scaler.pkl", "rb"))
model  = pickle.load(open("rf_raw.pkl", "rb"))
wrapper = RFWithExternalScaler(scaler, model, classes=["duduk","jalan","jatuh","tidur"])
pickle.dump(wrapper, open("models/imu/rf_activity_wrapper.pkl", "wb"), protocol=4)
```

### Contoh 3 — Model PPG (Klasifikasi HR)

```json
{
    "model_name": "hr_classifier_v1",
    "model_version": "1.0.0",
    "labels": ["normal", "tachycardia", "bradycardia"],
    "features": [
        {"name": "ir_mean",           "type": "stat", "signal": "ir", "stat": "mean",           "default": 0.0},
        {"name": "ir_std",            "type": "stat", "signal": "ir", "stat": "std",            "default": 0.0},
        {"name": "ir_rms",            "type": "stat", "signal": "ir", "stat": "rms",            "default": 0.0},
        {"name": "ir_peak_freq",      "type": "stat", "signal": "ir", "stat": "peak_freq",      "default": 0.0},
        {"name": "ir_spectral_energy","type": "stat", "signal": "ir", "stat": "spectral_energy","default": 0.0},
        {"name": "hr",                "type": "meta", "field": "hr",   "default": -1},
        {"name": "spo2",              "type": "meta", "field": "spo2", "default": 0.0}
    ],
    "skip_if": {
        "finger_required": true,
        "min_hr": 20
    },
    "output": {
        "confidence_threshold": 0.4
    }
}
```
