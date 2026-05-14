# =============================================================================
# server/core/config.py — Parameter terpusat
#
# EDIT FILE INI untuk menyesuaikan dengan setup hardware Anda.
# Semua apps di server/apps/ import dari sini — tidak perlu edit satu-satu.
# =============================================================================

# ── Parameter CS — HARUS sama dengan CS_Sensor.h di firmware ─────────────────
CS_N        = 64    # panjang window (jumlah sampel per sinyal)
CS_M        = 32    # jumlah pengukuran per window (50% kompresi)
CS_PHI_SEED = 42    # ⚠ KRITIS: harus identik dengan firmware, jangan diubah

# ── LASSO ─────────────────────────────────────────────────────────────────────
LASSO_ALPHA    = 0.001   # regularisasi — optimal untuk M=32, N=64
LASSO_MAX_ITER = 5000
LASSO_TOL      = 1e-5

# ── MQTT ──────────────────────────────────────────────────────────────────────
MQTT_BROKER   = "192.168.1.18"   # IP PC yang jalankan Mosquitto
MQTT_PORT     = 1883
MQTT_KEEPALIVE = 60
TOPIC_BASE    = "health_monitor"

# ── Sinyal yang direkonstruksi per window ─────────────────────────────────────
SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz", "ir"]
UNITS   = {
    "ax": "m/s²", "ay": "m/s²", "az": "m/s²",
    "gx": "deg/s", "gy": "deg/s", "gz": "deg/s",
    "ir": "ADC",
}

# ── Toleransi timestamp spread antar 7 paket dari 1 window (ms) ──────────────
# Setelah fix firmware (ts_now konsisten): spread harusnya < 50ms.
# 300ms untuk toleransi network jitter.
TS_SPREAD_TOLERANCE_MS = 300

# ── Visualizer ────────────────────────────────────────────────────────────────
HISTORY_WINDOWS = 5   # berapa window yang ditampilkan di rolling plot
MAX_HIST        = 60  # berapa titik di HR trend / kualitas sinyal chart

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}

# ── Derived (tidak perlu diubah) ──────────────────────────────────────────────
WINDOW_MS      = CS_N * 10   # durasi 1 window dalam ms (10ms = 100Hz IMU)
TOTAL_SAMPLES  = CS_N * HISTORY_WINDOWS