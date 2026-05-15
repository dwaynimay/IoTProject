# File: server/core/config.py

# =============================================================================
# config.py — Parameter Terpusat Server
# =============================================================================
#
# EDIT FILE INI untuk menyesuaikan setup hardware dan algoritma.
# Semua apps di server/apps/ import dari cs_router.py — tidak dari sini langsung.
#
# CARA GANTI ALGORITMA REKONSTRUKSI:
#   Ubah CS_ALGORITHM di bawah → restart server → selesai.
#   Tidak perlu ubah apps/ sama sekali.
# =============================================================================

# ── Algoritma Rekonstruksi ────────────────────────────────────────────────────
#
#   "omp"   → Hadamard-Gaussian Φ + DCT Ψ + OMP (default, tidak butuh sklearn)
#   "lasso" → Gaussian Φ + DCT Ψ + LASSO (butuh scikit-learn)
#
# ⚠️  Pastikan firmware menggunakan file CS_Sensor.h yang sesuai:
#   CS_ALGORITHM = "omp"   → firmware/include/CS_Sensor.h (Hadamard-Gaussian)
#   CS_ALGORITHM = "lasso" → archive/firmware/CS_Sensor_gaussian_lasso.h
CS_ALGORITHM = "omp"


# ── Parameter CS — HARUS sama dengan CS_Sensor.h di firmware ─────────────────
CS_N        = 64    # panjang window (jumlah sampel per sinyal)
CS_M        = 32    # jumlah pengukuran per window (50% kompresi)
CS_PHI_SEED = 42    # ⚠️ KRITIS: harus identik dengan firmware, jangan diubah


# ── OMP (Orthogonal Matching Pursuit) ────────────────────────────────────────
#
# K = jumlah komponen sparse yang dicari.
#
# Panduan tuning:
#   K terlalu kecil → rekonstruksi mulus tapi kehilangan detail tajam
#   K terlalu besar → bisa overfit ke noise, hasil jagged
#   Mulai dari K=10, naikkan 2-per-2 sambil pantau residual di visualizer.
#   Target residual: < 0.10 (hijau di test_single_signal.py)
OMP_K = 20


# ── LASSO (hanya aktif jika CS_ALGORITHM = "lasso") ──────────────────────────
#
# LASSO_ALPHA: regularisasi.
#   Lebih besar → lebih sparse/smooth, tapi kehilangan detail.
#   Lebih kecil → lebih detail, tapi bisa overfit ke noise.
#   Default 0.001 optimal untuk M=32, N=64.
LASSO_ALPHA    = 0.001
LASSO_MAX_ITER = 5000
LASSO_TOL      = 1e-5


# ── MQTT ──────────────────────────────────────────────────────────────────────
MQTT_BROKER    = "192.168.1.7"
MQTT_PORT      = 1883
MQTT_KEEPALIVE = 60
TOPIC_BASE     = "health_monitor"


# ── Sinyal yang direkonstruksi per window ─────────────────────────────────────
SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz", "ir"]
UNITS   = {
    "ax": "m/s²", "ay": "m/s²", "az": "m/s²",
    "gx": "deg/s", "gy": "deg/s", "gz": "deg/s",
    "ir": "ADC",
}

# ── Toleransi timestamp spread antar 7 paket dari 1 window ───────────────────
TS_SPREAD_TOLERANCE_MS = 300


# ── Visualizer ────────────────────────────────────────────────────────────────
HISTORY_WINDOWS = 5
MAX_HIST        = 60

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}


# ── Derived — tidak perlu diubah ──────────────────────────────────────────────
WINDOW_MS     = CS_N * 10        # durasi 1 window dalam ms (asumsi 100Hz IMU)
TOTAL_SAMPLES = CS_N * HISTORY_WINDOWS