# ML Models Directory

Folder ini berisi semua model ML yang dipakai engine inferensi.

## Struktur

```
models/
├── imu/         ← Model berbasis sinyal IMU (accelerometer, gyroscope)
├── ppg/         ← Model berbasis sinyal PPG (photoplethysmography / detak jantung)
└── README.md
```

## Konvensi File

Setiap model membutuhkan **2 file** di folder yang sama:

| File | Keterangan |
|------|-----------|
| `<name>.pkl` | Model sklearn-compatible (harus punya `predict_proba()`) |
| `<name>_config.json` | Manifest: labels, features, skip_if |

Contoh untuk model IMU:
```
models/imu/
├── fall_detection_svm_wrapper.pkl
└── fall_detection_svm_wrapper_config.json
```

## Cara Tambah Model Baru

1. Letakkan `.pkl` dan `_config.json` di subfolder yang sesuai (`imu/` atau `ppg/`)
2. Panggil `registry.scan("models/", recursive=True)` — model otomatis terdeteksi
3. Atau hot-register saat runtime: `registry.register("models/imu/new_model.pkl", "models/imu/new_model_config.json")`

Tidak perlu registrasi path manual — cukup taruh file dan scan.

## Model yang Tersedia

| Model | Folder | Labels | Fitur |
|-------|--------|--------|-------|
| `fall_detection_svm_wrapper` | `imu/` | duduk, jalan, jatuh, tidur | 44 IMU features (ax/ay/az + SMV + cross) |
