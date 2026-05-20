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

# ── Parameter CS — HARUS sama dengan CS_Sensor.h di firmware ──────────────
CS_N        = 64
CS_M        = 32
CS_PHI_SEED = 42

# ── OMP ───────────────────────────────────────────────────────────────────
OMP_K = 20

# ── LASSO ────────────────────────────────────────────────────────────────
LASSO_ALPHA    = 0.001
LASSO_MAX_ITER = 5000
LASSO_TOL      = 1e-5

# ── MQTT ──────────────────────────────────────────────────────────────────
MQTT_BROKER    = "10.129.25.254"
MQTT_PORT      = 1883
MQTT_KEEPALIVE = 60
TOPIC_BASE     = "health_monitor"

# ── Hybrid Topic — sesuai refactor gateway ────────────────────────────────
# Gateway sekarang publish 2 topic per node per window:
#   cs_imu → 6 sinyal IMU (ax,ay,az,gx,gy,gz) dalam 1 pesan
#   cs_ppg → sinyal IR + metadata HR
IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
PPG_SIGNALS = ["ir"]
SIGNALS     = IMU_SIGNALS + PPG_SIGNALS  # untuk kompatibilitas kode lama

# ── Unit per sinyal ───────────────────────────────────────────────────────
UNITS = {
    "ax": "m/s²", "ay": "m/s²", "az": "m/s²",
    "gx": "deg/s", "gy": "deg/s", "gz": "deg/s",
    "ir": "ADC",
}

# ── Toleransi timestamp spread antar cs_imu dan cs_ppg ───────────────────
# cs_imu dan cs_ppg dikirim hampir bersamaan dari gateway
# toleransi 500ms cukup untuk jitter jaringan
TS_SPREAD_TOLERANCE_MS = 500

# ── Visualizer ───────────────────────────────────────────────────────────
HISTORY_WINDOWS = 5
MAX_HIST        = 60

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}

# ── Derived ───────────────────────────────────────────────────────────────
WINDOW_MS     = CS_N * 10
TOTAL_SAMPLES = CS_N * HISTORY_WINDOWS

# ── Storage ───────────────────────────────────────────────────────────────────
DB_PATH         = "health_monitor.db"
RETENTION_HOURS = 24