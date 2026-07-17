# File: server/core/config.py

# =============================================================================
# config.py — Konfigurasi Terpusat Server
# =============================================================================
#
# Semua nilai dibaca dari environment variable.
# Untuk development lokal, buat file server/.env dari server/.env.example.
#
# CARA SETUP:
#   cp server/.env.example server/.env
#   # Edit server/.env sesuai environment Anda
#
# CARA GANTI ALGORITMA REKONSTRUKSI:
#   Set CS_ALGORITHM=lasso di .env → restart server → selesai.
#   Tidak perlu ubah apps/ sama sekali.
#
# ATURAN:
#   - Tidak ada nilai sensitif (IP, credential) yang hardcode di file ini.
#   - Semua nilai prod harus di-set via environment variable OS / container.
#   - Nilai default di sini HARUS aman untuk development lokal.
# =============================================================================

import os
from pathlib import Path

# Load .env jika ada (untuk development lokal).
# Di production, env var di-set langsung oleh OS / container — dotenv di-skip.
try:
    from dotenv import load_dotenv
    _env_path = Path(__file__).parent.parent / ".env"
    if _env_path.exists():
        load_dotenv(_env_path)
except ImportError:
    # python-dotenv opsional. Jika tidak terinstall, env var harus di-set manual.
    pass


def _get(key: str, default: str) -> str:
    """Ambil env var dengan fallback ke default yang aman untuk lokal."""
    return os.environ.get(key, default)


def _get_int(key: str, default: int) -> int:
    """Ambil env var sebagai int."""
    try:
        return int(os.environ.get(key, str(default)))
    except ValueError:
        return default


def _get_float(key: str, default: float) -> float:
    """Ambil env var sebagai float."""
    try:
        return float(os.environ.get(key, str(default)))
    except ValueError:
        return default


# =============================================================================
# MQTT
# =============================================================================
MQTT_BROKER    = _get("MQTT_BROKER", "localhost")
MQTT_PORT      = _get_int("MQTT_PORT", 1883)
MQTT_KEEPALIVE = _get_int("MQTT_KEEPALIVE", 60)
TOPIC_BASE     = _get("TOPIC_BASE", "health_monitor")

# =============================================================================
# CS Algorithm
# =============================================================================
#
# "hadamard" → Hadamard Φ + DCT Ψ + OMP (default, tidak butuh sklearn)
#
# ⚠️  Pastikan firmware menggunakan CS_Sensor.h yang sesuai:
#   CS_ALGORITHM = "hadamard"   → lib/CS_Model_Hadamard/CS_Sensor.h
CS_ALGORITHM = _get("CS_ALGORITHM", "hadamard")

# =============================================================================
# Parameter CS — HARUS sama dengan CS_Sensor.h di firmware
# =============================================================================
CS_N        = 64   # panjang window — harus pangkat 2
CS_M        = 32   # jumlah pengukuran (50% kompresi)
CS_PHI_SEED = 0    # seed Φ — HARUS identik dengan CS_PHI_SEED di firmware
OMP_K       = 20   # sparsity level OMP

# =============================================================================
# Logging
# =============================================================================
# Dikontrol via LOG_LEVEL env var. Default INFO untuk production.
# Set LOG_LEVEL=DEBUG di .env untuk melihat detail internal.
LOG_LEVEL = _get("LOG_LEVEL", "INFO")

# =============================================================================
# Storage SQLite
# =============================================================================
DB_PATH         = _get("DB_PATH", "server/data/health_monitor.db")
RETENTION_HOURS = _get_int("RETENTION_HOURS", 24)

# =============================================================================
# Signal metadata (tidak berubah — bukan credential)
# =============================================================================
IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
PPG_SIGNALS = ["ir"]
SIGNALS     = IMU_SIGNALS + PPG_SIGNALS

UNITS = {
    "ax": "m/s²", "ay": "m/s²", "az": "m/s²",
    "gx": "deg/s", "gy": "deg/s", "gz": "deg/s",
    "ir": "ADC",
}

TS_SPREAD_TOLERANCE_MS = _get_int("TS_SPREAD_TOLERANCE_MS", 500)

# =============================================================================
# Node Grouping — 1 sensor per ESP
# =============================================================================
# Saat firmware menggunakan 1 sensor per ESP (bukan 2 sensor per ESP),
# server perlu mempasangkan data cs_imu dari satu node dengan cs_ppg
# dari node lain menjadi 1 window rekonstruksi.
#
# Format: group_id → { "imu_node": node_id, "ppg_node": node_id }
#   - group_id   : ID virtual yang dipakai untuk output (storage, dashboard)
#   - imu_node   : node_id fisik yang kirim cs_imu
#   - ppg_node   : node_id fisik yang kirim cs_ppg
#
# Jika NODE_GROUPS kosong ({}), server fallback ke behavior lama
# (cs_imu + cs_ppg harus dari node yang sama).
NODE_GROUPS: dict[int, dict[str, int]] = {
    1: {"imu_node": 1, "ppg_node": 2},
}

# =============================================================================
# Visualizer (tidak berubah)
# =============================================================================
HISTORY_WINDOWS = 5
MAX_HIST        = 60

COLORS = {
    "ax": "#2196F3", "ay": "#4CAF50", "az": "#FF9800",
    "gx": "#9C27B0", "gy": "#F44336", "gz": "#00BCD4",
    "ir": "#E91E63",
}

# =============================================================================
# Derived (dihitung dari konstanta di atas)
# =============================================================================
WINDOW_MS     = CS_N * 10        # durasi satu window dalam ms (64 sampel × 10ms)
TOTAL_SAMPLES = CS_N * HISTORY_WINDOWS  # total sampel di rolling history