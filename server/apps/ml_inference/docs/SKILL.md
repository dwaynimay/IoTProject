---
name: ml-engine-pipeline
description: >
  Gunakan skill ini setiap kali user ingin membuat, melatih, atau menyesuaikan model ML
  agar kompatibel dengan MLInferenceEngine. Trigger skill ini ketika user menyebut:
  "buat pipeline", "training model baru", "sesuaikan dengan engine", "buat notebook training",
  "model untuk dataset lain", "ganti classifier", "buat config JSON otomatis", "deploy model ke engine",
  atau variasi apapun yang melibatkan training model dan integrasi ke inference engine.
  Skill ini menghasilkan Google Colab notebook (.ipynb) yang siap dijalankan — output berupa
  sklearn Pipeline langsung (tanpa wrapper) + _config.json yang di-generate otomatis.
  Selalu gunakan skill ini bahkan jika user hanya menyebut "buat model" atau "training" dalam
  konteks sistem sensor/IMU/PPG mereka.
---

# ML Engine Pipeline Generator

Skill ini menghasilkan **Google Colab notebook** yang:
1. Melatih model ML dari dataset apapun
2. Menyimpan output sebagai `sklearn Pipeline` langsung — **tanpa perlu wrapper**
3. Meng-generate `_config.json` **otomatis** dari FEATURE_SCHEMA yang sama dengan training
4. Memvalidasi engine compatibility sebelum menyimpan

---

## Prinsip Utama — WAJIB DIPAHAMI SEBELUM GENERATE

### A. Kontrak Engine (Non-Negotiable)

Engine (`MLInferenceEngine`) punya kontrak keras yang **tidak boleh dilanggar**:

```
model.predict_proba(X)
  X     : ndarray (n_samples, n_features), dtype float64
  return: ndarray (n_samples, n_classes),  dtype float64, sum per row ≈ 1.0
```

Model yang valid: `sklearn Pipeline` atau `ImbPipeline` dimana **step terakhir** adalah classifier dengan `probability=True`. Simpan langsung dengan `pickle.dump(pipeline, f, protocol=4)` — **bukan dict**.

### B. FEATURE_SCHEMA sebagai Single Source of Truth (SSOT)

**Aturan terpenting:** satu variabel `FEATURE_SCHEMA` harus dipakai untuk DUA hal sekaligus:
1. Mendefinisikan urutan fitur di `extract_features()` saat training
2. Mengisi array `"features"` di `_config.json` yang di-generate otomatis

Ini mencegah mismatch antara urutan fitur training vs inferensi engine. Jangan pernah pisahkan keduanya.

```python
# BENAR — SSOT pattern
FEATURE_SCHEMA = [
    ("ax_mean", {"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"}),
    # dst...
]

# training pakai FEATURE_SCHEMA
def extract_features(win):
    feats = []
    # urutan komputasi harus IDENTIK dengan urutan FEATURE_SCHEMA
    feats.append(np.mean(win[:,0]))  # ax_mean
    # dst...
    return np.array(feats, dtype=np.float64)

# config JSON pakai FEATURE_SCHEMA yang sama
config["features"] = [cfg for _, cfg in FEATURE_SCHEMA]
```

### C. Urutan Kelas Mengikuti sklearn (Alphabetical)

`LabelEncoder.fit_transform()` mengurutkan kelas secara **alphabetical**. Urutan `"labels"` di config JSON **harus sama** dengan `model.classes_` yang merupakan `le.classes_`.

```python
le = LabelEncoder()
y  = le.fit_transform(y_raw)
CLASSES = list(le.classes_)   # ['duduk','jalan','jatuh','tidur'] — alphabetical
# ...
config["labels"] = CLASSES   # BUKAN hardcode manual
```

---

## Alur Kerja Saat Generate Notebook

### Step 1 — Interview User

Kumpulkan informasi berikut sebelum generate. Jika sudah ada di konteks percakapan, ekstrak langsung — jangan tanya ulang.

**Wajib ditanyakan jika belum ada:**

| Informasi | Contoh | Default |
|-----------|--------|---------|
| Jenis dataset & formatnya | CSV per file, HDF5, satu CSV besar | — |
| Sinyal yang tersedia | ax, ay, az / gx, gy, gz / ir | — |
| Kelas target | duduk, jalan, jatuh, tidur | — |
| Window size | 64 (CS engine), 100, 128 | 64 |
| Sampling rate (Hz) | 20, 50, 100 | — |
| Classifier yang diinginkan | SVM, RandomForest, GradientBoosting | SVM |
| Apakah ada imbalance? | ya → tanyakan strategi | SMOTE jika ya |
| Nama model & versi | fall_detection_v1 | — |
| Path Google Drive | MyDrive/project/dataset.zip | — |

**Fitur yang diinginkan:**
Tanyakan fitur mana yang relevan untuk sinyal tersebut. Lihat referensi fitur di bawah.

### Step 2 — Pilih Template

Berdasarkan jawaban user, pilih template yang sesuai:

| Kondisi | Template | Baca |
|---------|----------|------|
| Classifier = SVM | SVM + SMOTE + GridSearch | [`references/template-svm.md`] |
| Classifier = RandomForest | RF + class_weight | [`references/template-rf.md`] |
| Classifier = GradientBoosting | GBM / XGBoost | [`references/template-gbm.md`] |
| Dataset = satu CSV besar dengan kolom label | — | Adaptasi loader di Step 3 |
| Dataset = banyak file CSV per aktivitas | — | Pakai pattern LABEL_MAP |
| Sinyal = IMU (ax/ay/az/gx/gy/gz) | — | Fitur: stat + smv/smv_gyro + cross + zcr |
| Sinyal = PPG (ir + hr + spo2) | — | Fitur: stat ir + peak_freq + meta |
| Sinyal = gabungan IMU + PPG | — | Gabungkan kedua set fitur |

### Step 3 — Generate Notebook

Buat notebook dengan **12 step standar** berikut. Baca [`references/notebook-structure.md`] untuk template lengkap setiap step.

```
STEP  0 — Install & Import
STEP  1 — Mount Google Drive & Setup Path
STEP  2 — Konfigurasi (WINDOW_SIZE, LABEL_MAP, metadata)
STEP  3 — Load Dataset & Filter
STEP  4 — FEATURE_SCHEMA + extract_features() + Sliding Window
STEP  5 — Visualisasi Distribusi & Sanity Check Sinyal
STEP  6 — Train/Test Split (stratified 80/20)
STEP  7 — Training Pipeline (baseline CV + GridSearchCV)
STEP  8 — Evaluasi Test Set + Confusion Matrix
STEP  9 — Verifikasi Engine Compatibility  ← JANGAN SKIP
STEP 10 — Simpan .pkl + Auto-generate _config.json
STEP 11 — Smoke Test (simulasi inferensi engine)
STEP 12 — Laporan Akhir
```

---

## Panduan FEATURE_SCHEMA per Jenis Sinyal

### IMU — Accelerometer (ax, ay, az)

```python
FEATURE_SCHEMA = []

# Stat per axis
for sig in ['ax', 'ay', 'az']:
    for stat, cfg_stat in [
        ('mean','mean'), ('std','std'), ('min','min'), ('max','max'),
        ('range','range'), ('skew','skew'), ('kurt','kurt'),
        ('rms','rms'), ('q1','p25'), ('q3','p75'),
    ]:
        FEATURE_SCHEMA.append((
            f'{sig}_{stat}',
            {"name": f'{sig}_{stat}', "type": "stat", "signal": sig, "stat": cfg_stat}
        ))

# SMV (Signal Magnitude Vector = sqrt(ax²+ay²+az²))
for stat, cfg_stat in [('mean','mean'),('std','std'),('max','max'),('min','min'),
                        ('range','range'),('skew','skew'),('kurt','kurt'),('rms','rms')]:
    FEATURE_SCHEMA.append((
        f'smv_{stat}',
        {"name": f'smv_{stat}', "type": "derived", "formula": "smv", "stat": cfg_stat}
    ))

# Zero Crossing Rate
for sig in ['ax', 'ay', 'az']:
    FEATURE_SCHEMA.append((
        f'{sig}_zcr',
        {"name": f'{sig}_zcr', "type": "stat", "signal": sig, "stat": "zcr"}
    ))

# Korelasi cross-axis
for sa, sb, nm in [('ax','ay','corr_xy'),('ax','az','corr_xz'),('ay','az','corr_yz')]:
    FEATURE_SCHEMA.append((
        nm,
        {"name": nm, "type": "cross", "signal_a": sa, "signal_b": sb,
         "cross_type": "corr", "default": 0.0}
    ))
# Total: 44 fitur
```

**extract_features() yang sesuai:**
```python
def extract_features(win):
    ax, ay, az = win[:,0], win[:,1], win[:,2]
    smv = np.sqrt(ax**2 + ay**2 + az**2)
    feats = []
    for a in [ax, ay, az]:
        feats += [np.mean(a), np.std(a), np.min(a), np.max(a),
                  float(np.max(a)-np.min(a)), float(stats.skew(a)),
                  float(stats.kurtosis(a)),   # Fisher/excess
                  float(np.sqrt(np.mean(a**2))),
                  float(np.percentile(a,25)), float(np.percentile(a,75))]
    feats += [np.mean(smv), np.std(smv), np.max(smv), np.min(smv),
              float(np.max(smv)-np.min(smv)), float(stats.skew(smv)),
              float(stats.kurtosis(smv)), float(np.sqrt(np.mean(smv**2)))]
    for a in [ax, ay, az]:
        feats.append(float(np.sum(np.diff(np.sign(a)) != 0) / len(a)))
    feats += [float(np.corrcoef(ax,ay)[0,1]),
              float(np.corrcoef(ax,az)[0,1]),
              float(np.corrcoef(ay,az)[0,1])]
    return np.array(feats, dtype=np.float64)
```

### IMU — Gyroscope (gx, gy, gz)

Sama seperti accelerometer, ganti sinyal ke `gx/gy/gz` dan formula SMV ke `smv_gyro`:
```python
{"name": "smv_gyro_mean", "type": "derived", "formula": "smv_gyro", "stat": "mean"}
```

### PPG — Infrared + HR + SpO2

```python
FEATURE_SCHEMA = []

# Stat sinyal IR
for stat, cfg_stat in [('mean','mean'),('std','std'),('min','min'),('max','max'),
                        ('rms','rms'),('range','range'),('skew','skew'),('kurt','kurt'),
                        ('peak_freq','peak_freq'),('spectral_energy','spectral_energy')]:
    FEATURE_SCHEMA.append((
        f'ir_{stat}',
        {"name": f'ir_{stat}', "type": "stat", "signal": "ir", "stat": cfg_stat}
    ))

# Metadata HR dan SpO2
FEATURE_SCHEMA.append(('hr',   {"name":"hr",   "type":"meta","field":"hr",   "default":-1}))
FEATURE_SCHEMA.append(('spo2', {"name":"spo2", "type":"meta","field":"spo2", "default":0.0}))
```

**Catatan `peak_freq` dan `spectral_energy`:** engine menghitung ini via FFT pada sinyal rekonstruksi. Di notebook, hitung manual agar konsisten:
```python
from scipy.fft import rfft, rfftfreq

def peak_freq(sig, fs):
    freqs = rfftfreq(len(sig), 1/fs)
    mag   = np.abs(rfft(sig))
    return float(freqs[np.argmax(mag[1:])+1])

def spectral_energy(sig):
    return float(np.sum(np.abs(rfft(sig))**2) / len(sig))
```

### Gabungan IMU + PPG (Fusion)

Gabungkan FEATURE_SCHEMA dari kedua jenis sinyal secara berurutan. Model disimpan di `models/fusion/`.

---

## Panduan Classifier

### SVM (default — cocok untuk dataset kecil-menengah, <50K windows)

```python
from imblearn.pipeline import Pipeline as ImbPipeline
from imblearn.over_sampling import SMOTE
from sklearn.svm import SVC
from sklearn.preprocessing import StandardScaler

pipeline = ImbPipeline([
    ('scaler', StandardScaler()),
    ('smote',  SMOTE(random_state=42, k_neighbors=3)),  # hapus jika balanced
    ('svm',    SVC(kernel='rbf', class_weight='balanced',
                   random_state=42, probability=True))   # probability=True WAJIB
])

param_grid = {
    'svm__C'    : [0.1, 1, 10, 100],
    'svm__gamma': ['scale', 'auto', 0.01, 0.001],
}
```

### RandomForest (cocok untuk dataset besar, interpretable)

```python
from sklearn.pipeline import Pipeline
from sklearn.ensemble import RandomForestClassifier
from sklearn.preprocessing import StandardScaler

pipeline = Pipeline([
    ('scaler', StandardScaler()),
    ('clf',    RandomForestClassifier(class_weight='balanced',
                                      random_state=42, n_jobs=-1))
])

param_grid = {
    'clf__n_estimators': [100, 200, 300],
    'clf__max_depth'   : [None, 10, 20],
    'clf__min_samples_split': [2, 5],
}
```

**Catatan RF:** tidak perlu SMOTE karena ada `class_weight='balanced'`. Gunakan sklearn Pipeline biasa, bukan ImbPipeline.

### GradientBoosting / XGBoost

Baca [`references/template-gbm.md`] untuk implementasi detail.

---

## Panduan Handling Imbalance

| Rasio max/min | Strategi yang direkomendasikan |
|---------------|-------------------------------|
| ≤ 2x | Tidak perlu — gunakan `class_weight='balanced'` saja |
| 2x – 5x | SMOTE di dalam pipeline + `class_weight='balanced'` |
| > 5x | SMOTE + undersampling (imblearn Pipeline dengan `RandomUnderSampler`) |
| Kelas minoritas < 10 sample | Jangan SMOTE (k_neighbors tidak cukup) — cari lebih banyak data |

**SMOTE harus selalu di dalam pipeline** agar tidak bocor ke validation fold:
```python
# BENAR — SMOTE di dalam ImbPipeline
ImbPipeline([('scaler', ...), ('smote', SMOTE(...)), ('clf', ...)])

# SALAH — SMOTE diluar, bocor ke validation
X_res, y_res = SMOTE().fit_resample(X_train, y_train)
pipeline.fit(X_res, y_res)
```

---

## Panduan Loader Dataset

### Format: banyak file CSV per aktivitas (UMA-style)

```python
LABEL_MAP = [
    ('keyword_aktivitas_1', 'nama_kelas'),
    ('keyword_aktivitas_2', 'nama_kelas'),
    ('aktivitas_skip',      None),         # None = skip file ini
]

def get_label(filepath):
    fname = os.path.basename(filepath).lower()
    for keyword, label in LABEL_MAP:
        if keyword in fname:
            return label
    return None
```

### Format: satu CSV besar dengan kolom label

```python
df = pd.read_csv('dataset.csv')
# Pastikan ada kolom: sinyal + label
# Filter kelas yang diinginkan
TARGET_CLASSES = ['kelas_a', 'kelas_b', 'kelas_c']
df = df[df['label'].isin(TARGET_CLASSES)]

# Group per sequence (jika ada kolom session/subject)
records = []
for (subject, session), grp in df.groupby(['subject', 'session']):
    data = grp[['ax','ay','az']].values.astype(np.float32)
    lbl  = grp['label'].iloc[0]
    records.append((data, lbl))
```

### Format: HDF5 / Parquet

Baca [`references/loaders.md`] untuk implementasi detail.

---

## Step 9 — Verifikasi Engine Compatibility (WAJIB, Jangan Skip)

Cell ini harus selalu ada sebelum simpan model:

```python
print('── Verifikasi Engine Compatibility ──')

# 1. predict_proba ada?
assert hasattr(best_model, 'predict_proba'), '✗ predict_proba tidak ada!'
print('✓ predict_proba() tersedia')

# 2. Output shape benar?
X_dummy = np.random.randn(5, N_FEATURES).astype(np.float64)
proba   = best_model.predict_proba(X_dummy)
assert proba.shape == (5, len(CLASSES)), f'✗ Shape salah: {proba.shape}'
print(f'✓ Output shape: {proba.shape}')

# 3. Probabilitas sum ke 1?
assert np.allclose(proba.sum(axis=1), 1.0, atol=1e-5), '✗ Proba tidak sum ke 1!'
print(f'✓ Proba sum ≈ 1.0')

# 4. Kelas konsisten?
clf_step   = best_model.steps[-1][1]  # step terakhir
clf_classes = list(clf_step.classes_)
assert clf_classes == CLASSES, f'✗ Mismatch: clf={clf_classes}, CLASSES={CLASSES}'
print(f'✓ Urutan kelas konsisten: {CLASSES}')

# 5. Jumlah fitur = FEATURE_SCHEMA?
assert N_FEATURES == len(FEATURE_SCHEMA)
print(f'✓ Jumlah fitur: {N_FEATURES}')

print('\n✅ Semua cek passed — model siap disimpan')
```

---

## Step 10 — Simpan (Pattern Standar)

```python
import datetime

# A. Simpan model — LANGSUNG Pipeline, bukan dict
pickle.dump(best_model, open(f'/content/{MODEL_FILENAME}', 'wb'), protocol=4)

# B. Generate config JSON dari FEATURE_SCHEMA (SSOT)
config = {
    "model_name"    : MODEL_NAME,
    "model_version" : MODEL_VERSION,
    "description"   : "...",
    "author"        : AUTHOR,
    "trained_at"    : datetime.datetime.now().strftime('%Y-%m'),
    "labels"        : CLASSES,                              # dari LabelEncoder
    "features"      : [cfg for _, cfg in FEATURE_SCHEMA],  # dari SSOT
    "skip_if"       : {"require_signals": [...] },
    "output"        : {"confidence_threshold": CONFIDENCE_THRESHOLD},
    "_training_info": {                                     # informatif, tidak dibaca engine
        "window_size"  : WINDOW_SIZE,
        "best_params"  : grid_search.best_params_,
        "test_accuracy": round(acc, 4),
        "test_f1_macro": round(f1m, 4),
    }
}

with open(f'/content/{CONFIG_FILENAME}', 'w') as f:
    json.dump(config, f, indent=4, ensure_ascii=False)

# C. Copy ke Google Drive
for fname in [MODEL_FILENAME, CONFIG_FILENAME]:
    shutil.copy(f'/content/{fname}', os.path.join(DRIVE_OUT_DIR, fname))
```

---

## Checklist Final Sebelum Generate

```
[ ] Tahu format dataset (banyak CSV / satu CSV / HDF5)
[ ] Tahu sinyal yang tersedia (IMU / PPG / fusion)
[ ] Tahu kelas target dan mapping dari nama file/kolom
[ ] Window size sesuai engine (CS_N=64 atau sesuai kebutuhan)
[ ] FEATURE_SCHEMA dan extract_features() urutan identik
[ ] Classifier dipilih sesuai ukuran dataset
[ ] Strategi imbalance ditentukan (class_weight / SMOTE / keduanya)
[ ] Step 9 (verifikasi compatibility) ada di notebook
[ ] Model disimpan sebagai Pipeline langsung, bukan dict
[ ] Config JSON di-generate dari FEATURE_SCHEMA yang sama
[ ] Folder tujuan benar: imu/ untuk IMU, ppg/ untuk PPG, fusion/ untuk gabungan
```

---

## Referensi

- [`references/notebook-structure.md`] — Template lengkap setiap step notebook
- [`references/engine-contracts.md`] — Kontrak lengkap engine: fitur yang didukung, sinyal, skip_if
- [`references/template-rf.md`] — Template RandomForest pipeline
- [`references/template-gbm.md`] — Template GradientBoosting/XGBoost pipeline
- [`references/loaders.md`] — Loader untuk HDF5, Parquet, satu CSV besar
