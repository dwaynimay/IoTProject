# File: server/core/validator.py

# =============================================================================
# validator.py — Payload Validator (F1)
# =============================================================================
#
# 5 layer validasi sebelum payload diteruskan ke rekonstruksi CS:
#
#   Layer 1 — Schema   : field wajib ada di payload
#   Layer 2 — Length   : len(y) == CS_M untuk setiap sinyal
#   Layer 3 — Finite   : tidak ada NaN / Inf dalam measurement vector
#   Layer 4 — Monotonicity : ts tidak boleh mundur per node (anti-replay)
#   Layer 5 — Whitelist: node_id harus ada di ALLOWED_NODE_IDS (opsional)
#
# CARA PAKAI di reconstruct_server.py:
#
#   from core.validator import ValidatorRegistry, ValidationError
#
#   registry = ValidatorRegistry()
#
#   # saat terima cs_imu:
#   ok, errors = registry.validate_imu(node_id=1, payload=imu_payload)
#   if not ok:
#       print(errors)  # list string deskriptif
#       return
#
#   # saat terima cs_ppg:
#   ok, errors = registry.validate_ppg(node_id=1, payload=ppg_payload)
#
# KONFIGURASI:
#   ALLOWED_NODE_IDS  → set node ID yang diizinkan, None = nonaktif (terima semua)
#   TS_MAX_JUMP_MS    → toleransi loncat ts ke depan (reboot / clock skew)
#   MEASUREMENT_ABS_MAX → batas absolut nilai y[i] sebelum dianggap corrupt
# =============================================================================

from __future__ import annotations

import logging
import math
from typing import Optional

logger = logging.getLogger(__name__)

# =============================================================================
# Konfigurasi
# =============================================================================

# Set berisi node ID yang diizinkan.
# Isi dengan int, contoh: {1, 2}. Set ke None untuk menerima semua node.
ALLOWED_NODE_IDS: Optional[set[int]] = None  # None = nonaktif

# Jika ts melompat lebih dari nilai ini ke DEPAN, dianggap reboot atau
# clock skew — tetap diterima tapi dicatat.
# Jika ts mundur lebih dari TS_BACKWARD_TOLERANCE_MS, paket ditolak.
TS_MAX_JUMP_MS: int        = 60_000   # 60 detik ke depan = toleransi
TS_BACKWARD_TOLERANCE_MS: int = 100   # 100 ms ke belakang = masih oke (jitter)

# Batas absolut nilai elemen measurement vector y[i].
# Nilai di luar range ini hampir pasti corrupt (overflow, NaN propagation).
MEASUREMENT_ABS_MAX: float = 1e6

# Jumlah M pengukuran per sinyal — harus sinkron dengan config.py CS_M
# Di-import dari config agar tidak ada duplikasi konstanta.
try:
    from .config import CS_M, IMU_SIGNALS, PPG_SIGNALS
except ImportError:
    # Fallback jika dijalankan standalone (misalnya untuk unit test)
    CS_M        = 32
    IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
    PPG_SIGNALS = ["ir"]


# =============================================================================
# Field wajib per topic type
# =============================================================================

# cs_imu: timestamp + finger flag + 6 sinyal IMU
_IMU_REQUIRED_FIELDS: tuple[str, ...] = ("ts", "finger") + tuple(IMU_SIGNALS)

# cs_ppg: timestamp + metadata HR + finger flag + sinyal IR
_PPG_REQUIRED_FIELDS: tuple[str, ...] = ("ts", "hr", "finger") + tuple(PPG_SIGNALS)


# =============================================================================
# ValidationError — exception opsional (bisa diabaikan, cukup cek ok/errors)
# =============================================================================

class ValidationError(Exception):
    """
    Dilempar jika ingin validasi gagal menghentikan eksekusi secara eksplisit.
    Gunakan pattern ok, errors = registry.validate_*() untuk flow normal.
    """
    def __init__(self, errors: list[str]):
        self.errors = errors
        super().__init__("; ".join(errors))


# =============================================================================
# _MonotonicityTracker — state per node untuk cek ts
# =============================================================================

class _MonotonicityTracker:
    """
    Simpan ts terakhir yang valid per node.
    Instance ini dimiliki oleh ValidatorRegistry — tidak perlu dibuat manual.
    """

    def __init__(self) -> None:
        # key: node_id (int), value: ts terakhir yang valid (int, ms)
        self._last_ts: dict[int, int] = {}

    def check(self, node_id: int, ts: int) -> tuple[bool, str]:
        """
        Cek apakah ts valid dibandingkan ts terakhir untuk node ini.

        Returns:
            (True, "")           → ts valid, state diupdate
            (False, pesan_error) → ts mundur, state TIDAK diupdate
        """
        if node_id not in self._last_ts:
            # Node baru — langsung diterima
            self._last_ts[node_id] = ts
            return True, ""

        prev = self._last_ts[node_id]
        diff = ts - prev  # positif = maju, negatif = mundur

        if diff < -TS_BACKWARD_TOLERANCE_MS:
            # Ts mundur signifikan — tolak
            return False, (
                f"ts mundur: node={node_id} "
                f"prev={prev}ms current={ts}ms diff={diff}ms "
                f"(tolerance={TS_BACKWARD_TOLERANCE_MS}ms)"
            )

        if diff > TS_MAX_JUMP_MS:
            # Lompat jauh ke depan — kemungkinan reboot, tetap diterima
            # tapi update state dan beri warning (bukan error)
            self._last_ts[node_id] = ts
            return True, f"[WARN] ts lompat besar: node={node_id} diff={diff}ms (kemungkinan reboot)"

        # Normal
        self._last_ts[node_id] = ts
        return True, ""

    def reset(self, node_id: int) -> None:
        """Reset state node tertentu (misalnya saat reboot terdeteksi)."""
        self._last_ts.pop(node_id, None)

    def reset_all(self) -> None:
        """Reset semua state — berguna saat server restart."""
        self._last_ts.clear()


# =============================================================================
# Layer validator functions — murni fungsi, tidak ada state
# =============================================================================

def _layer1_schema(payload: dict, required: tuple[str, ...]) -> list[str]:
    """Layer 1: cek field wajib ada."""
    missing = [f for f in required if f not in payload]
    return [f"schema: field wajib tidak ada: {missing}"] if missing else []


def _layer2_length(payload: dict, signals: list[str]) -> list[str]:
    """Layer 2: cek panjang measurement vector == CS_M."""
    errors = []
    for sig in signals:
        val = payload.get(sig)
        if not isinstance(val, (list, tuple)):
            errors.append(f"length: '{sig}' bukan list (type={type(val).__name__})")
            continue
        if len(val) != CS_M:
            errors.append(
                f"length: '{sig}' panjang {len(val)}, diharapkan {CS_M}"
            )
    return errors


def _layer3_finite(payload: dict, signals: list[str]) -> list[str]:
    """
    Layer 3: cek setiap elemen y[i] finite (tidak NaN / Inf) dan dalam batas.

    Berhenti setelah temukan elemen pertama yang invalid per sinyal
    agar error message tidak membanjiri log.
    """
    errors = []
    for sig in signals:
        val = payload.get(sig)
        if not isinstance(val, (list, tuple)):
            continue  # sudah ditangani layer 2

        for i, v in enumerate(val):
            try:
                fv = float(v)
            except (TypeError, ValueError):
                errors.append(f"finite: '{sig}[{i}]' tidak bisa dikonversi ke float: {v!r}")
                break

            if not math.isfinite(fv):
                errors.append(f"finite: '{sig}[{i}]' = {fv} (NaN atau Inf)")
                break

            if abs(fv) > MEASUREMENT_ABS_MAX:
                errors.append(
                    f"finite: '{sig}[{i}]' = {fv:.3e} "
                    f"melebihi batas absolut ±{MEASUREMENT_ABS_MAX:.0e}"
                )
                break

    return errors


def _layer5_whitelist(node_id: int) -> list[str]:
    """Layer 5: cek node_id ada di whitelist (jika whitelist aktif)."""
    if ALLOWED_NODE_IDS is None:
        return []
    if node_id not in ALLOWED_NODE_IDS:
        return [
            f"whitelist: node_id={node_id} tidak diizinkan "
            f"(allowed={sorted(ALLOWED_NODE_IDS)})"
        ]
    return []


# =============================================================================
# ValidatorRegistry — state + semua layer dalam satu objek
# =============================================================================

class ValidatorRegistry:
    """
    Registry validasi dengan state per-node untuk monotonicity check.

    Buat satu instance di level modul/app, lalu pakai terus.
    Thread-safety: _MonotonicityTracker tidak thread-safe.
    Jika dipanggil dari multiple thread, bungkus dengan threading.Lock() di luar.

    Contoh:
        registry = ValidatorRegistry()
        ok, errors = registry.validate_imu(node_id=1, payload=data)
    """

    def __init__(self) -> None:
        self._tracker = _MonotonicityTracker()

        # Counter statistik untuk monitoring
        self.stats: dict[str, int] = {
            "imu_ok":      0,
            "imu_invalid": 0,
            "ppg_ok":      0,
            "ppg_invalid": 0,
        }

    # ── Public API ────────────────────────────────────────────────────────────

    def validate_imu(
        self,
        node_id: int,
        payload: dict,
    ) -> tuple[bool, list[str]]:
        """
        Validasi payload cs_imu.

        Args:
            node_id : ID node pengirim (dari MQTT topic)
            payload : dict hasil json.loads(message.payload)

        Returns:
            (True, [])           → valid, siap rekonstruksi
            (False, list[str])   → invalid, list pesan error
        """
        errors, warnings = self._run_layers(
            node_id   = node_id,
            payload   = payload,
            required  = _IMU_REQUIRED_FIELDS,
            signals   = list(IMU_SIGNALS),
        )

        if warnings:
            for w in warnings:
                logger.warning("Node %d | %s", node_id, w)

        if errors:
            self.stats["imu_invalid"] += 1
            return False, errors

        self.stats["imu_ok"] += 1
        return True, []

    def validate_ppg(
        self,
        node_id: int,
        payload: dict,
    ) -> tuple[bool, list[str]]:
        """
        Validasi payload cs_ppg.

        Args:
            node_id : ID node pengirim
            payload : dict hasil json.loads(message.payload)

        Returns:
            (True, [])           → valid
            (False, list[str])   → invalid
        """
        errors, warnings = self._run_layers(
            node_id   = node_id,
            payload   = payload,
            required  = _PPG_REQUIRED_FIELDS,
            signals   = list(PPG_SIGNALS),
        )

        if warnings:
            for w in warnings:
                logger.warning("Node %d | %s", node_id, w)

        if errors:
            self.stats["ppg_invalid"] += 1
            return False, errors

        self.stats["ppg_ok"] += 1
        return True, []

    def get_stats(self) -> dict[str, int]:
        """Kembalikan salinan counter statistik."""
        return dict(self.stats)

    def reset_node(self, node_id: int) -> None:
        """Reset state monotonicity untuk node tertentu."""
        self._tracker.reset(node_id)

    # ── Internal ──────────────────────────────────────────────────────────────

    def _run_layers(
        self,
        node_id: int,
        payload: dict,
        required: tuple[str, ...],
        signals: list[str],
    ) -> tuple[list[str], list[str]]:
        """
        Jalankan semua layer validasi secara berurutan.
        Berhenti lebih awal jika layer sebelumnya sudah gagal
        (menghindari error cascade yang membingungkan).

        Returns:
            (errors, warnings) — keduanya list[str]
        """
        errors:   list[str] = []
        warnings: list[str] = []

        # Layer 5 — whitelist (cek node_id dulu, paling murah)
        errors += _layer5_whitelist(node_id)
        if errors:
            return errors, warnings

        # Layer 1 — schema (field wajib ada sebelum akses field lain)
        errors += _layer1_schema(payload, required)
        if errors:
            return errors, warnings

        # Layer 4 — monotonicity (ts ada di payload, baru bisa dicek)
        ts = payload.get("ts")
        if not isinstance(ts, (int, float)):
            errors.append(f"monotonicity: 'ts' bukan angka (type={type(ts).__name__})")
            return errors, warnings

        ts_int = int(ts)
        mono_ok, mono_msg = self._tracker.check(node_id, ts_int)
        if not mono_ok:
            errors.append(f"monotonicity: {mono_msg}")
            return errors, warnings
        elif mono_msg:  # warning dari lompat besar
            warnings.append(mono_msg)

        # Layer 2 — length
        errors += _layer2_length(payload, signals)
        if errors:
            return errors, warnings

        # Layer 3 — finite (paling mahal, jalankan paling akhir)
        errors += _layer3_finite(payload, signals)

        return errors, warnings
