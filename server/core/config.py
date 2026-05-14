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

# ── OMP (Orthogonal Matching Pursuit) ────────────────────────────────────────
# K = jumlah komponen sparse yang dicari (sparsity level)
#
# Aturan praktis untuk sinyal IMU/PPG nyata:
#   K ≈ M / (2 · log(N/M)) = 32 / (2 · log(64/32)) ≈ 23
#   Tapi empiris K=10–15 sudah cukup untuk gerakan manusia sehari-hari.
#
# Panduan tuning:
#   K terlalu kecil → rekonstruksi mulus tapi kehilangan detail tajam
#   K terlalu besar → bisa overfit ke noise, rekonstruksi jagged
#   Mulai dari K=10, naikkan 2-per-2 sambil lihat output visualizer
OMP_K = 20

# ── MQTT ──────────────────────────────────────────────────────────────────────
MQTT_BROKER    = "192.168.1.7"   # IP PC yang jalankan Mosquitto
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

# ── Toleransi timestamp spread antar 7 paket dari 1 window (ms) ──────────────
TS_SPREAD_TOLERANCE_MS = 300

# ── Visualizer ────────────────────────────────────────────────────────────────
HISTORY_WINDOWS = 5
MAX_HIST        = 60

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}

# ── Derived (tidak perlu diubah) ──────────────────────────────────────────────
WINDOW_MS     = CS_N * 10
TOTAL_SAMPLES = CS_N * HISTORY_WINDOWS
