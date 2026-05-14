# Archive — File Versi Lama

Folder ini menyimpan file dari versi sebelumnya sebagai referensi.
**Tidak ada file di sini yang aktif dipakai** — semua sudah digantikan oleh versi baru.

> PlatformIO tidak compile folder ini. Python tidak import dari sini.
> Aman disimpan tanpa memengaruhi project.

---

## Isi

### `firmware/CS_Sensor_gaussian_lasso.h`

Versi lama `CS_Sensor.h` sebelum ganti metode CS.

| Komponen     | Versi lama                       | Versi baru                 |
| ------------ | -------------------------------- | -------------------------- |
| Matriks Φ    | Gaussian acak (LCG + Box-Muller) | Hadamard-Gaussian (D · H)  |
| Basis Ψ      | DCT                              | Fourier / IDFT             |
| Rekonstruksi | LASSO (scikit-learn)             | OMP (implementasi sendiri) |

Disimpan untuk referensi jika ingin membandingkan performa atau kembali ke metode lama.

### `server/cs_utils_lasso.py`

Versi lama `server/core/cs_utils.py` — generate_phi Gaussian + rekonstruksi LASSO.

### `server/config_lasso.py`

Versi lama `server/core/config.py` — berisi parameter `LASSO_ALPHA`, `LASSO_MAX_ITER`, `LASSO_TOL`.

---

## Cara Kembali ke Versi Lama (jika diperlukan)

```bash
# Firmware
cp archive/firmware/CS_Sensor_gaussian_lasso.h include/CS_Sensor.h

# Server
cp archive/server/cs_utils_lasso.py server/core/cs_utils.py
cp archive/server/config_lasso.py   server/core/config.py

# Install scikit-learn (diperlukan LASSO)
pip install scikit-learn>=1.0.0
```

Lalu compile ulang firmware dan restart server Python.
