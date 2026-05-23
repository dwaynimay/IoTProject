# Notebook Structure — Template Lengkap per Step

Dokumen ini berisi template kode untuk setiap step notebook. Salin dan adaptasi sesuai kebutuhan.

---

## STEP 0 — Install & Import

```python
# Sesuaikan paket yang dibutuhkan berdasarkan classifier yang dipilih
!pip install scikit-learn imbalanced-learn scipy -q
# Tambahkan jika perlu: !pip install xgboost lightgbm -q

import os, re, zipfile, pickle, json, warnings, datetime, shutil
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
warnings.filterwarnings('ignore')

from scipy import stats
from scipy.signal import butter, filtfilt
from scipy.fft import rfft, rfftfreq       # untuk peak_freq / spectral_energy
from collections import Counter

from sklearn.svm import SVC
from sklearn.ensemble import RandomForestClassifier, GradientBoostingClassifier
from sklearn.preprocessing import StandardScaler, LabelEncoder
from sklearn.pipeline import Pipeline
from sklearn.model_selection import (
    StratifiedKFold, cross_validate, GridSearchCV, train_test_split
)
from sklearn.metrics import (
    classification_report, confusion_matrix, ConfusionMatrixDisplay,
    roc_auc_score, accuracy_score, f1_score
)
from imblearn.over_sampling import SMOTE
from imblearn.pipeline import Pipeline as ImbPipeline

print('✅ Import OK')
```

---

## STEP 1 — Mount Google Drive & Setup Path

```python
from google.colab import drive
drive.mount('/content/drive')

# ── SESUAIKAN SEMUA PATH DI SINI ─────────────────────────────────────────────
SOURCE_ZIP    = '/content/drive/MyDrive/<project>/dataset.zip'
EXTRACT_DIR   = '/content/Dataset'
DRIVE_OUT_DIR = '/content/drive/MyDrive/<project>/output/'

MODEL_FILENAME  = '<model_name>.pkl'
CONFIG_FILENAME = '<model_name>_config.json'

os.makedirs(DRIVE_OUT_DIR, exist_ok=True)

# Jika dataset berupa ZIP:
!cp "{SOURCE_ZIP}" "dataset.zip"
os.makedirs(EXTRACT_DIR, exist_ok=True)
with zipfile.ZipFile('dataset.zip', 'r') as z:
    z.extractall(EXTRACT_DIR)

# Ekstrak ZIP bersarang (jika ada)
changed = True
while changed:
    changed = False
    for root, dirs, files in os.walk(EXTRACT_DIR):
        for f in files:
            if f.endswith('.zip'):
                zp  = os.path.join(root, f)
                out = zp.replace('.zip', '')
                os.makedirs(out, exist_ok=True)
                with zipfile.ZipFile(zp, 'r') as z2:
                    z2.extractall(out)
                os.remove(zp)
                changed = True

all_csv = []
for root, dirs, files in os.walk(EXTRACT_DIR):
    for f in files:
        if f.lower().endswith('.csv'):
            all_csv.append(os.path.join(root, f))

print(f'✅ Ekstraksi selesai — {len(all_csv)} file CSV ditemukan')
```

---

## STEP 2 — Konfigurasi

```python
# ── Sensor / window settings ──────────────────────────────────────────────────
FS          = 20      # Sampling rate (Hz)
WINDOW_SIZE = 64      # ⚠ Engine CS_N=64. Ganti jika bukan CS pipeline.
STEP_SIZE   = 32      # 50% overlap

# ── Label mapping (format banyak file CSV) ────────────────────────────────────
# Key  : substring dari nama file (case-insensitive)
# Value: nama kelas target (None = skip file ini)
# PENTING: urutan matching penting — keyword lebih spesifik taruh di atas
LABEL_MAP = [
    ('keyword_kelas_a', 'kelas_a'),
    ('keyword_kelas_b', 'kelas_b'),
    ('keyword_skip',     None),
]

# ── Model metadata ────────────────────────────────────────────────────────────
MODEL_NAME           = '<model_name>_v1'
MODEL_VERSION        = '1.0.0'
AUTHOR               = '<team>'
CONFIDENCE_THRESHOLD = 0.30

print('✅ Konfigurasi OK')
print(f'   Window : {WINDOW_SIZE} sampel = {WINDOW_SIZE/FS:.1f}s @ {FS}Hz')
```

---

## STEP 3 — Load Dataset & Filter

### Format banyak CSV per aktivitas:

```python
def get_label(filepath):
    fname = os.path.basename(filepath).lower()
    for keyword, label in LABEL_MAP:
        if keyword in fname:
            return label
    return None

def read_signals(fp):
    """Sesuaikan parsing dengan format CSV dataset."""
    try:
        df = pd.read_csv(fp, ...)  # sesuaikan separator, header, dsb
        # Filter sensor yang diinginkan jika ada kolom sensor_id / sensor_type
        signals = df[['ax','ay','az']].dropna()
        if len(signals) < WINDOW_SIZE:
            return None
        return signals.values.astype(np.float32)
    except:
        return None

def lowpass(sig, cutoff=5.0, fs=20.0, order=4):
    b, a = butter(order, cutoff/(0.5*fs), btype='low')
    return filtfilt(b, a, sig)

records, n_skipped = [], 0
for fp in all_csv:
    lbl  = get_label(fp)
    if lbl is None:
        n_skipped += 1
        continue
    data = read_signals(fp)
    if data is None:
        n_skipped += 1
        continue
    # Filter setiap axis
    data_filt = np.column_stack([lowpass(data[:,i], fs=FS) for i in range(data.shape[1])])
    records.append((data_filt, lbl))

print(f'✅ Berhasil : {len(records)} file | Dilewati : {n_skipped}')
for k, v in sorted(Counter(r[1] for r in records).items()):
    print(f'   {k}: {v} file')
```

### Format satu CSV besar:

```python
df = pd.read_csv('data.csv')
TARGET_CLASSES = ['kelas_a', 'kelas_b', 'kelas_c']
df = df[df['label'].isin(TARGET_CLASSES)]

records = []
# Group berdasarkan subject/session jika ada, atau sliding window langsung
for lbl, grp in df.groupby('label'):
    data = grp[['ax','ay','az']].values.astype(np.float32)
    records.append((data, lbl))
```

---

## STEP 4 — FEATURE_SCHEMA + extract_features() + Sliding Window

```python
# ── FEATURE_SCHEMA (SSOT) ─────────────────────────────────────────────────────
# Sesuaikan dengan sinyal yang tersedia dan fitur yang diinginkan.
# Lihat SKILL.md bagian "Panduan FEATURE_SCHEMA" untuk opsi lengkap.

FEATURE_SCHEMA = []
# ... (isi sesuai sinyal, lihat SKILL.md)

N_FEATURES = len(FEATURE_SCHEMA)
print(f'Total fitur: {N_FEATURES}')

# Tampilkan schema
for i, (name, cfg) in enumerate(FEATURE_SCHEMA):
    print(f'  [{i:>3}] {name}')
```

```python
def extract_features(win):
    """
    URUTAN KOMPUTASI HARUS IDENTIK DENGAN URUTAN FEATURE_SCHEMA.
    win: ndarray shape (WINDOW_SIZE, n_signals)
    return: ndarray shape (N_FEATURES,) dtype float64
    """
    feats = []
    # ... (implementasi sesuai FEATURE_SCHEMA)
    return np.array(feats, dtype=np.float64)

# Sliding window
X_list, y_list = [], []
for data, lbl in records:
    for start in range(0, len(data) - WINDOW_SIZE + 1, STEP_SIZE):
        win = data[start : start + WINDOW_SIZE]
        X_list.append(extract_features(win))
        y_list.append(lbl)

X     = np.array(X_list, dtype=np.float64)
y_raw = np.array(y_list)

# Encode label (sklearn sort alphabetical → urutan di config JSON)
le      = LabelEncoder()
y       = le.fit_transform(y_raw)
CLASSES = list(le.classes_)

# Bersihkan NaN/Inf
X = np.nan_to_num(X, nan=0.0, posinf=0.0, neginf=0.0)

win_dist = Counter(y_raw)
print(f'✅ X shape: {X.shape} | Kelas: {CLASSES}')
for lbl, cnt in sorted(win_dist.items()):
    pct = 100*cnt/len(y_raw)
    print(f'   {lbl}: {cnt} ({pct:.1f}%)')
ratio = max(win_dist.values()) / min(win_dist.values())
print(f'   Rasio imbalance: {ratio:.1f}x')
```

---

## STEP 5 — Visualisasi

```python
labels_s = sorted(win_dist.keys())
counts_s = [win_dist[l] for l in labels_s]
colors   = ['#1565C0','#2E7D32','#E65100','#B71C1C','#6A1B9A','#00695C'][:len(labels_s)]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4))
bars = ax1.bar(labels_s, counts_s, color=colors, edgecolor='white')
ax1.bar_label(bars, padding=3)
ax1.set_title('Distribusi Window per Label', fontweight='bold')
ax1.set_ylabel('Jumlah Window')
ax1.grid(axis='y', alpha=0.3)
ax2.pie(counts_s, labels=labels_s, autopct='%1.1f%%', colors=colors,
        startangle=90, wedgeprops={'edgecolor':'white'})
ax2.set_title('Proporsi Label', fontweight='bold')
plt.tight_layout()
plt.savefig('/content/distribusi_label.png', dpi=130)
plt.show()
```

---

## STEP 6 — Train/Test Split

```python
X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.20, random_state=42, stratify=y
)
print(f'Train: {X_train.shape[0]} | Test: {X_test.shape[0]}')
for i, cls in enumerate(CLASSES):
    print(f'  {cls}: train={np.sum(y_train==i)}, test={np.sum(y_test==i)}')
```

---

## STEP 7 — Training (SVM default)

```python
# Baseline CV
baseline_pipe = ImbPipeline([
    ('scaler', StandardScaler()),
    ('smote',  SMOTE(random_state=42, k_neighbors=3)),
    ('svm',    SVC(kernel='rbf', C=1.0, gamma='scale',
                   class_weight='balanced', random_state=42, probability=True))
])
cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=42)
res = cross_validate(baseline_pipe, X_train, y_train, cv=cv,
                     scoring=['accuracy','f1_macro'], n_jobs=-1)
print(f'Baseline — Accuracy: {res["test_accuracy"].mean():.4f} | F1: {res["test_f1_macro"].mean():.4f}')
```

```python
# GridSearchCV
param_grid = {
    'svm__C'    : [0.1, 1, 10, 100],
    'svm__gamma': ['scale', 'auto', 0.01, 0.001],
}
tune_pipe = ImbPipeline([
    ('scaler', StandardScaler()),
    ('smote',  SMOTE(random_state=42, k_neighbors=3)),
    ('svm',    SVC(kernel='rbf', class_weight='balanced', random_state=42, probability=True))
])
grid_search = GridSearchCV(
    tune_pipe, param_grid,
    cv=StratifiedKFold(n_splits=5, shuffle=True, random_state=42),
    scoring='f1_macro', n_jobs=-1, verbose=1
)
grid_search.fit(X_train, y_train)
print(f'Best params : {grid_search.best_params_}')
print(f'Best F1 CV  : {grid_search.best_score_:.4f}')
best_model = grid_search.best_estimator_
```

---

## STEP 8 — Evaluasi

```python
y_pred = best_model.predict(X_test)
y_prob = best_model.predict_proba(X_test)
acc    = accuracy_score(y_test, y_pred)
f1m    = f1_score(y_test, y_pred, average='macro')
f1w    = f1_score(y_test, y_pred, average='weighted')

print(classification_report(y_test, y_pred, target_names=CLASSES, digits=4))
print(f'Accuracy: {acc:.4f} | F1 Macro: {f1m:.4f} | F1 Weighted: {f1w:.4f}')

try:
    auc = roc_auc_score(y_test, y_prob, multi_class='ovr', average='macro')
    print(f'AUC OvR: {auc:.4f}')
except Exception as e:
    print(f'AUC: {e}')

fig, axes = plt.subplots(1, 2, figsize=(14, 5))
ConfusionMatrixDisplay(confusion_matrix(y_test,y_pred), display_labels=CLASSES).plot(
    ax=axes[0], colorbar=False, cmap='Blues')
axes[0].set_title('Count', fontweight='bold')
ConfusionMatrixDisplay(np.round(confusion_matrix(y_test,y_pred,normalize='true'),2),
                       display_labels=CLASSES).plot(ax=axes[1], colorbar=False, cmap='Greens')
axes[1].set_title('Normalized', fontweight='bold')
plt.tight_layout()
plt.savefig('/content/confusion_matrix.png', dpi=130)
plt.show()
```

---

## STEP 9 — Verifikasi Engine Compatibility

```python
print('── Verifikasi Engine Compatibility ──')

assert hasattr(best_model, 'predict_proba'), '✗ predict_proba tidak ada!'
print('✓ predict_proba() tersedia')

X_dummy = np.random.randn(5, N_FEATURES).astype(np.float64)
proba   = best_model.predict_proba(X_dummy)
assert proba.shape == (5, len(CLASSES)), f'✗ Shape salah: {proba.shape}'
print(f'✓ Output shape: {proba.shape}')

assert np.allclose(proba.sum(axis=1), 1.0, atol=1e-5)
print('✓ Proba sum ≈ 1.0')

clf_step    = best_model.steps[-1][1]
clf_classes = list(clf_step.classes_)
assert clf_classes == CLASSES, f'✗ Mismatch: {clf_classes} vs {CLASSES}'
print(f'✓ Urutan kelas konsisten: {CLASSES}')

assert N_FEATURES == len(FEATURE_SCHEMA)
print(f'✓ Jumlah fitur: {N_FEATURES}')

print('\n✅ Semua cek passed')
```

---

## STEP 10 — Simpan .pkl + Auto-generate Config JSON

```python
MODEL_PATH  = f'/content/{MODEL_FILENAME}'
CONFIG_PATH = f'/content/{CONFIG_FILENAME}'

# A. Simpan model — LANGSUNG Pipeline
pickle.dump(best_model, open(MODEL_PATH, 'wb'), protocol=4)
print(f'✅ Model: {MODEL_PATH} ({os.path.getsize(MODEL_PATH)/1024:.1f} KB)')

# B. Generate config JSON dari FEATURE_SCHEMA (SSOT)
config = {
    "model_name"    : MODEL_NAME,
    "model_version" : MODEL_VERSION,
    "description"   : f"...",
    "author"        : AUTHOR,
    "trained_at"    : datetime.datetime.now().strftime('%Y-%m'),
    "labels"        : CLASSES,
    "features"      : [cfg for _, cfg in FEATURE_SCHEMA],
    "skip_if"       : {"require_signals": ["ax", "ay", "az"]},
    "output"        : {"confidence_threshold": CONFIDENCE_THRESHOLD},
    "_training_info": {
        "window_size": WINDOW_SIZE, "step_size": STEP_SIZE, "fs_hz": FS,
        "n_features": N_FEATURES, "n_train": int(X_train.shape[0]),
        "n_test": int(X_test.shape[0]), "test_accuracy": round(acc,4),
        "test_f1_macro": round(f1m,4),
    }
}
with open(CONFIG_PATH, 'w') as f:
    json.dump(config, f, indent=4, ensure_ascii=False)
print(f'✅ Config: {CONFIG_PATH}')
print(json.dumps({k:v for k,v in config.items() if k!='_training_info'}, indent=2, ensure_ascii=False))

# C. Copy ke Google Drive
for fname in [MODEL_FILENAME, CONFIG_FILENAME, 'confusion_matrix.png', 'distribusi_label.png']:
    src = f'/content/{fname}'
    if os.path.exists(src):
        shutil.copy(src, os.path.join(DRIVE_OUT_DIR, fname))
        print(f'✅ Copied: {fname}')
```

---

## STEP 11 — Smoke Test

```python
model_loaded = pickle.load(open(MODEL_PATH, 'rb'))
X_sim  = X_test[:3].astype(np.float64)
proba  = model_loaded.predict_proba(X_sim)
labels = model_loaded.predict(X_sim)

print(f'Input  : {X_sim.shape} | Output: {proba.shape}')
for i in range(len(X_sim)):
    pred_label = CLASSES[labels[i]]
    confidence = proba[i].max()
    proba_str  = ' | '.join([f'{CLASSES[j]}:{proba[i,j]:.3f}' for j in range(len(CLASSES))])
    print(f'  Sample {i}: {pred_label} ({confidence:.1%}) — {proba_str}')

print(f'\nclasses_       : {list(model_loaded.steps[-1][1].classes_)}')
print(f'n_features_in_ : {getattr(model_loaded, "n_features_in_", "N/A")}')
print('✅ Smoke test selesai')
```

---

## STEP 12 — Laporan Akhir

```python
print('='*58)
print('              LAPORAN MODEL FINAL')
print('='*58)
clf_name = type(best_model.steps[-1][1]).__name__
print(f'  Classifier        : {clf_name}')
if hasattr(grid_search, 'best_params_'):
    print(f'  Best params       : {grid_search.best_params_}')
print(f'  Window size       : {WINDOW_SIZE} sampel ({WINDOW_SIZE/FS:.1f}s @ {FS}Hz)')
print(f'  Jumlah fitur      : {N_FEATURES}')
print(f'  Kelas             : {CLASSES}')
print()
print(f'  ── Test Set ──')
print(f'  Accuracy          : {acc:.4f} ({acc*100:.2f}%)')
print(f'  F1 Macro          : {f1m:.4f}')
print(f'  F1 Weighted       : {f1w:.4f}')
print()
print(f'  ── Engine Compatibility ──')
print(f'  Format            : sklearn Pipeline (tanpa wrapper)')
print(f'  Output files      : {MODEL_FILENAME}')
print(f'                      {CONFIG_FILENAME}')
print(f'  Deploy ke         : models/imu/  (atau ppg/ / fusion/)')
print('='*58)
```
