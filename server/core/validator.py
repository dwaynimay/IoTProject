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
TS_MAX_JUMP_MS: int           = 60_000   # 60 detik ke depan = toleransi
TS_BACKWARD_TOLERANCE_MS: int = 100      # 100 ms ke belakang = masih oke (jitter)
# Jika ts mundur lebih dari ini, dianggap ESP32 reboot → reset tracker, paket diterima.
TS_REBOOT_THRESHOLD_MS: int   = 5_000    # ts mundur > 5 detik = reboot (sebelumnya 30000)

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
_IMU_REQUIRED_FIELDS: tuple[str, ...] = (
    ("ts", "finger")
    + tuple(IMU_SIGNALS)
    + tuple(f"mean_{sig}" for sig in IMU_SIGNALS)
)

# cs_ppg: timestamp + metadata HR + finger flag + sinyal IR
_PPG_REQUIRED_FIELDS: tuple[str, ...] = (
    "ts", "hr", "spo2", "ppg_valid", "finger", "mean_ir"
) + tuple(PPG_SIGNALS)


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

        if diff < -TS_REBOOT_THRESHOLD_MS:
            # Ts mundur sangat jauh -> ESP32 reboot (millis() reset ke 0)
            # Reset tracker agar sesi baru tidak terus ditolak
            self._last_ts[node_id] = ts
            return True, (
                f"[WARN] reboot terdeteksi: node={node_id} "
                f"prev={prev}ms current={ts}ms diff={diff}ms "
                f"-- tracker direset"
            )

        if diff < -TS_BACKWARD_TOLERANCE_MS:
            # Ts mundur signifikan tapi bukan reboot -- tolak (jitter/replay)
            return False, (
                f"ts mundur: node={node_id} "
                f"prev={prev}ms current={ts}ms diff={diff}ms "
                f"(tolerance={TS_BACKWARD_TOLERANCE_MS}ms)"
            )

        if diff > TS_MAX_JUMP_MS:
            # Lompat jauh ke depan -- kemungkinan clock skew, tetap diterima
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


def _layer_metadata(payload: dict, signals: list[str], *, is_ppg: bool) -> list[str]:
    """Validate scalar metadata before reconstruction or ML conversion."""
    errors: list[str] = []

    ts = payload.get("ts")
    if isinstance(ts, bool) or not isinstance(ts, (int, float)):
        errors.append(f"metadata: 'ts' bukan angka (type={type(ts).__name__})")
    elif not math.isfinite(float(ts)) or float(ts) < 0:
        errors.append(f"metadata: 'ts' tidak valid: {ts!r}")

    if not isinstance(payload.get("finger"), bool):
        errors.append("metadata: 'finger' harus boolean")

    for sig in signals:
        key = f"mean_{sig}"
        value = payload.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            errors.append(f"metadata: '{key}' bukan angka")
        elif not math.isfinite(float(value)) or abs(float(value)) > MEASUREMENT_ABS_MAX:
            errors.append(f"metadata: '{key}' tidak finite atau di luar batas")

    if is_ppg:
        hr = payload.get("hr")
        if isinstance(hr, bool) or not isinstance(hr, (int, float)):
            errors.append("metadata: 'hr' bukan angka")
        elif not math.isfinite(float(hr)) or not -1 <= float(hr) <= 255:
            errors.append(f"metadata: 'hr' di luar range: {hr!r}")

        spo2 = payload.get("spo2")
        if spo2 is not None:
            if isinstance(spo2, bool) or not isinstance(spo2, (int, float)):
                errors.append("metadata: 'spo2' harus angka atau null")
            elif not math.isfinite(float(spo2)) or not 0 <= float(spo2) <= 100:
                errors.append(f"metadata: 'spo2' di luar range: {spo2!r}")

        if not isinstance(payload.get("ppg_valid"), bool):
            errors.append("metadata: 'ppg_valid' harus boolean")

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
            is_ppg    = False,
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
            is_ppg    = True,
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
        is_ppg: bool,
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
        # Layer 2 — length
        errors += _layer2_length(payload, signals)
        if errors:
            return errors, warnings

        # Layer 3 — finite (paling mahal, jalankan paling akhir)
        errors += _layer3_finite(payload, signals)
        errors += _layer_metadata(payload, signals, is_ppg=is_ppg)
        if errors:
            return errors, warnings

        # Commit timestamp only after all stateless validation has passed.
        ts_int = int(payload["ts"])
        mono_ok, mono_msg = self._tracker.check(node_id, ts_int)
        if not mono_ok:
            errors.append(f"monotonicity: {mono_msg}")
        elif mono_msg:
            warnings.append(mono_msg)

        return errors, warnings
