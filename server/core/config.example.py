# File: server/core/config.example.py

# =============================================================================
# config.example.py — Contoh Konfigurasi Eksplisit
# =============================================================================
#
# File ini adalah TEMPLATE untuk developer baru yang ingin memahami
# semua nilai konfigurasi tanpa perlu baca config.py + .env sekaligus.
#
# File ini AMAN di-commit karena tidak berisi nilai sensitif.
# Semua nilai di sini adalah contoh untuk development lokal.
#
# CARA PAKAI (jika ingin run tanpa .env):
#   # Opsi 1 — Gunakan .env (DIREKOMENDASIKAN):
#   cp server/.env.example server/.env
#   # Edit server/.env sesuai setup Anda
#
#   # Opsi 2 — Set langsung via environment:
#   export MQTT_BROKER=192.168.1.100
#   python -m server
#
# Konsisten dengan pola firmware: credentials.h.example di firmware =
# config.example.py di server.
# =============================================================================

# ── MQTT Broker ───────────────────────────────────────────────────────────────
MQTT_BROKER    = "localhost"       # ← ganti dengan IP broker Anda
MQTT_PORT      = 1883              # port default Mosquitto / EMQX
MQTT_KEEPALIVE = 60                # interval keepalive dalam detik
TOPIC_BASE     = "health_monitor"  # prefix semua topic MQTT

# ── CS Algorithm ──────────────────────────────────────────────────────────────
CS_ALGORITHM = "omp"   # "omp" (default) atau "lasso"

# ── Parameter CS — HARUS sama dengan firmware ─────────────────────────────────
CS_N        = 64   # panjang window (pangkat 2)
CS_M        = 32   # jumlah pengukuran — 50% kompresi
CS_PHI_SEED = 42   # seed matrix Φ — HARUS identik antara firmware dan server
OMP_K       = 20   # sparsity level OMP

# ── LASSO (hanya relevan jika CS_ALGORITHM = "lasso") ────────────────────────
LASSO_ALPHA    = 0.001   # regularisasi LASSO — lebih besar = lebih smooth
LASSO_MAX_ITER = 5000    # maksimum iterasi solver
LASSO_TOL      = 1e-5    # toleransi konvergensi

# ── Logging ───────────────────────────────────────────────────────────────────
LOG_LEVEL = "INFO"   # DEBUG | INFO | WARNING | ERROR | CRITICAL

# ── Storage SQLite ────────────────────────────────────────────────────────────
DB_PATH         = "health_monitor.db"   # path file SQLite
RETENTION_HOURS = 24                    # data lebih lama dari ini akan di-purge

# ── Timestamp Spread Tolerance ────────────────────────────────────────────────
# Toleransi spread antara cs_imu dan cs_ppg dalam milidetik.
# Jika spread > nilai ini, salah satu buffer di-reset.
TS_SPREAD_TOLERANCE_MS = 500

# =============================================================================
# Konstanta turunan — tidak perlu diubah
# =============================================================================
IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
PPG_SIGNALS = ["ir"]
SIGNALS     = IMU_SIGNALS + PPG_SIGNALS

UNITS = {
    "ax": "m/s²", "ay": "m/s²", "az": "m/s²",
    "gx": "deg/s", "gy": "deg/s", "gz": "deg/s",
    "ir": "ADC",
}

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}

HISTORY_WINDOWS = 5
MAX_HIST        = 60
WINDOW_MS       = CS_N * 10
TOTAL_SAMPLES   = CS_N * HISTORY_WINDOWS
