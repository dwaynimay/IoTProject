# Loaders — Format Dataset Non-Standard

Gunakan dokumen ini ketika dataset bukan berupa banyak CSV per aktivitas.

---

## Format: Satu CSV Besar dengan Kolom Label

Dataset seperti WISDM, PAMAP2 versi pre-processed, UCI HAR processed.

```python
# Load dan filter
df = pd.read_csv('dataset.csv')
# atau: df = pd.read_csv('data.csv', sep=';') / sep='\t'

# Periksa kolom yang ada
print(df.columns.tolist())
print(df.head())
print(df['label'].unique())

TARGET_CLASSES = ['kelas_a', 'kelas_b', 'kelas_c']
df = df[df['label'].isin(TARGET_CLASSES)].copy()
df = df.dropna(subset=['ax','ay','az','label'])

# Group per subject/session jika ada
records = []
if 'subject' in df.columns and 'session' in df.columns:
    for (subj, sess), grp in df.groupby(['subject','session']):
        grp_sorted = grp.sort_values('timestamp') if 'timestamp' in grp.columns else grp
        data = grp_sorted[['ax','ay','az']].values.astype(np.float32)
        lbl  = grp_sorted['label'].mode()[0]   # label dominan
        if len(data) >= WINDOW_SIZE:
            records.append((data, lbl))
elif 'subject' in df.columns:
    for subj, grp in df.groupby('subject'):
        data = grp[['ax','ay','az']].values.astype(np.float32)
        lbl  = grp['label'].mode()[0]
        if len(data) >= WINDOW_SIZE:
            records.append((data, lbl))
else:
    # Tidak ada grouping — sliding window langsung dari seluruh dataframe
    # Perlu handle label per-baris (ambil label setiap window dari tengah window)
    data   = df[['ax','ay','az']].values.astype(np.float32)
    labels = df['label'].values
    X_list, y_list = [], []
    for start in range(0, len(data) - WINDOW_SIZE + 1, STEP_SIZE):
        win = data[start:start+WINDOW_SIZE]
        # Label dari tengah window (paling representatif)
        mid_lbl = labels[start + WINDOW_SIZE//2]
        X_list.append(extract_features(win))
        y_list.append(mid_lbl)
    # Skip bagian records → langsung ke X, y_raw di bawah
```

---

## Format: UCI HAR (pre-segmented, fitur sudah diekstrak)

UCI HAR sudah dalam bentuk feature matrix — tidak perlu sliding window.

```python
import numpy as np

# Load langsung feature matrix
X_train = np.loadtxt('UCI_HAR/train/X_train.txt')
X_test  = np.loadtxt('UCI_HAR/test/X_test.txt')
y_train = np.loadtxt('UCI_HAR/train/y_train.txt', dtype=int) - 1  # 1-indexed → 0-indexed
y_test  = np.loadtxt('UCI_HAR/test/y_test.txt',   dtype=int) - 1

# Label names
with open('UCI_HAR/activity_labels.txt') as f:
    activity_labels = {int(l.split()[0])-1: l.split()[1].lower() for l in f}

CLASSES = [activity_labels[i] for i in sorted(activity_labels)]
print(f'X_train: {X_train.shape} | Kelas: {CLASSES}')

# PENTING: jika pakai UCI HAR yang sudah pre-extracted,
# FEATURE_SCHEMA harus mencerminkan 561 fitur yang sudah ada
# Engine TIDAK bisa menghitung ini dari sinyal — model ini hanya bisa
# dipakai jika engine juga mengirim feature vector yang sama.
# Lebih baik: train ulang dari raw signals agar cocok dengan engine.
```

> ⚠ **Peringatan UCI HAR dan sejenisnya:** Dataset pre-extracted tidak kompatibel
> dengan engine karena engine hanya mengirim sinyal raw (ax/ay/az/...).
> Rekomendasikan user untuk training dari raw signals, bukan dari feature matrix pre-built.

---

## Format: HDF5 (.h5 / .hdf5)

Dataset ilmiah besar seperti CASAS, PhysioNet, atau custom recording.

```python
import h5py

with h5py.File('dataset.h5', 'r') as f:
    # Lihat struktur
    def print_structure(name, obj):
        print(name)
    f.visititems(print_structure)

# Load setelah tahu strukturnya
with h5py.File('dataset.h5', 'r') as f:
    records = []
    for subject in f.keys():
        for activity in f[subject].keys():
            data = f[subject][activity]['accelerometer'][:]  # sesuaikan path
            lbl  = activity.lower()
            if lbl in [l for _, l in LABEL_MAP if l is not None]:
                records.append((data.astype(np.float32), lbl))

print(f'Total records: {len(records)}')
```

---

## Format: Parquet

```python
import pandas as pd

df = pd.read_parquet('dataset.parquet')
print(df.dtypes)
print(df['label'].value_counts())

# Lanjutkan sama seperti format satu CSV besar di atas
```

---

## Format: Numpy Array (.npy / .npz)

```python
# .npy
data = np.load('signals.npy')    # shape: (N, T, C) atau (N, C, T)
labels_raw = np.load('labels.npy', allow_pickle=True)

# .npz
bundle = np.load('dataset.npz', allow_pickle=True)
print(bundle.files)   # lihat key yang tersedia
X_raw   = bundle['X']      # sesuaikan key
y_raw   = bundle['y']
CLASSES = list(bundle['classes'])

# Jika sudah dalam bentuk windows (N, T, C):
# Langsung extract features tanpa sliding window
X_list = [extract_features(X_raw[i]) for i in range(len(X_raw))]
X      = np.array(X_list, dtype=np.float64)
```

---

## Dataset dengan Multiple Sensor Rates

Jika dataset mengandung sensor dengan sampling rate berbeda:

```python
from scipy.signal import resample

TARGET_FS = 20   # Hz target (sesuai FS di konfigurasi)

def resync_to_target_fs(data, original_fs, target_fs, target_len):
    """Resample sinyal ke panjang target."""
    n_target = int(len(data) * target_fs / original_fs)
    return resample(data, n_target)

# Contoh: IMU di 50Hz, ingin 20Hz
data_50hz   = read_sensor(fp, sensor_id=1)         # 50Hz
data_20hz   = resync_to_target_fs(data_50hz, 50, 20, target_len=None)
records.append((data_20hz, lbl))
```

---

## Validasi Umum untuk Semua Format

```python
# Setelah build records, selalu validasi:
print(f'Total records : {len(records)}')
print(f'Distribusi    :')
for k, v in sorted(Counter(r[1] for r in records).items()):
    print(f'  {k}: {v}')

# Cek panjang data minimum
min_len = min(len(r[0]) for r in records)
max_len = max(len(r[0]) for r in records)
print(f'Panjang data  : min={min_len}, max={max_len} sampel')
if min_len < WINDOW_SIZE:
    print(f'⚠ Ada {sum(1 for r in records if len(r[0]) < WINDOW_SIZE)} records lebih pendek dari WINDOW_SIZE={WINDOW_SIZE}')
    records = [(d,l) for d,l in records if len(d) >= WINDOW_SIZE]
    print(f'   Setelah filter: {len(records)} records')
```
