# File: server/core/quality.py

# =============================================================================
# quality.py — Reconstruction Quality Assessment (F3)
# =============================================================================
#
# Hitung metrik kualitas SETELAH rekonstruksi CS selesai.
# Dipanggil dari reconstruct_server.py._reconstruct() per window.
#
# METRIK PER SINYAL:
#
#   relative_error  = ||y - Φ·x̂||₂ / ||y||₂
#       → Seberapa baik x̂ "menjelaskan" measurement y.
#         0.0 = sempurna, > 0.25 = buruk (flag LOW_QUALITY)
#         Rumus ini sudah familiar dari test_single_signal.py.
#
#   sparsity_ratio  = jumlah koefisien DCT ≠ 0 / N
#       → Fraksi koefisien yang dipakai OMP.
#         Harusnya kecil (sinyal sparse). Jika mendekati 1.0,
#         OMP membutuhkan hampir semua koefisien → sinyal tidak sparse
#         → rekonstruksi tidak optimal.
#
#   snr_db          = 20 · log10(||x̂||₂ / ||y - Φ·x̂||₂)
#       → Estimasi SNR dalam dB. Lebih tinggi = lebih baik.
#         Hanya informatif, bukan dipakai untuk flag.
#
# FLAG KUALITAS:
#   LOW_QUALITY  → relative_error > THRESHOLD_LOW_QUALITY (default 0.25)
#   ZERO_SIGNAL  → ||y||₂ < 1e-9 (sinyal hampir nol, tidak bisa dinilai)
#
# CARA PAKAI di reconstruct_server.py:
#
#   from core.quality import QualityAssessor, QualityFlag
#
#   assessor = QualityAssessor(phi=PHI)   # PHI dari cs_router
#
#   # setelah reconstruct semua sinyal:
#   report = assessor.assess_window(
#       results      = {"ax": x_hat_ax, "gx": x_hat_gx, ...},
#       measurements = {"ax": y_ax,     "gx": y_gx,     ...},
#   )
#
#   print(report.summary())       # satu baris ringkasan
#   if report.has_low_quality():
#       print(report.low_quality_signals())
#
# CARA PAKAI PER SINYAL:
#
#   metric = assessor.assess_signal(signal="ax", x_hat=x_hat, y=y_arr)
#   print(metric.relative_error, metric.flag)
# =============================================================================

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

import numpy as np

try:
    from .config import CS_M, CS_N, IMU_SIGNALS, PPG_SIGNALS
except ImportError:
    CS_M        = 32
    CS_N        = 64
    IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
    PPG_SIGNALS = ["ir"]

SIGNALS = IMU_SIGNALS + PPG_SIGNALS


# =============================================================================
# Konfigurasi threshold
# =============================================================================

# relative_error di atas ini → flag LOW_QUALITY
THRESHOLD_LOW_QUALITY: float = 0.25

# relative_error di atas ini → flag sangat buruk (untuk logging lebih keras)
THRESHOLD_CRITICAL: float = 0.50

# Koefisien dianggap nol jika |s_hat[i]| < nilai ini
SPARSITY_EPSILON: float = 1e-8

# ||y||₂ di bawah ini → sinyal dianggap nol (tidak dinilai)
ZERO_SIGNAL_THRESHOLD: float = 1e-9


# =============================================================================
# QualityFlag
# =============================================================================

class QualityFlag(str, Enum):
    OK           = "OK"           # relative_error <= 0.25
    LOW_QUALITY  = "LOW_QUALITY"  # relative_error > 0.25
    CRITICAL     = "CRITICAL"     # relative_error > 0.50
    ZERO_SIGNAL  = "ZERO_SIGNAL"  # ||y|| hampir nol, tidak bisa dinilai


# =============================================================================
# SignalMetric — hasil per sinyal
# =============================================================================

@dataclass
class SignalMetric:
    """Metrik kualitas untuk satu sinyal dalam satu window."""

    signal:          str
    relative_error:  float          # ||y - Φx̂|| / ||y||
    sparsity_ratio:  float          # fraksi koefisien ≠ 0
    snr_db:          float          # estimasi SNR dalam dB
    flag:            QualityFlag
    residual_norm:   float          # ||y - Φx̂||₂
    signal_norm:     float          # ||y||₂

    def is_ok(self) -> bool:
        return self.flag == QualityFlag.OK

    def is_low_quality(self) -> bool:
        return self.flag in (QualityFlag.LOW_QUALITY, QualityFlag.CRITICAL)

    def is_critical(self) -> bool:
        return self.flag == QualityFlag.CRITICAL

    def short_str(self) -> str:
        """Ringkasan satu baris untuk logging."""
        flag_tag = "" if self.flag == QualityFlag.OK else f" [{self.flag.value}]"
        return (
            f"{self.signal}: "
            f"err={self.relative_error:.4f} "
            f"sparse={self.sparsity_ratio:.2f} "
            f"snr={self.snr_db:+.1f}dB"
            f"{flag_tag}"
        )


# =============================================================================
# WindowReport — hasil seluruh window
# =============================================================================

@dataclass
class WindowReport:
    """
    Kumpulan SignalMetric untuk semua sinyal dalam satu window.
    Dibuat oleh QualityAssessor.assess_window().
    """

    node_id:    int
    window_num: int
    metrics:    dict[str, SignalMetric] = field(default_factory=dict)

    # ── Query helpers ─────────────────────────────────────────────────────────

    def has_low_quality(self) -> bool:
        return any(m.is_low_quality() for m in self.metrics.values())

    def has_critical(self) -> bool:
        return any(m.is_critical() for m in self.metrics.values())

    def low_quality_signals(self) -> list[str]:
        return [s for s, m in self.metrics.items() if m.is_low_quality()]

    def critical_signals(self) -> list[str]:
        return [s for s, m in self.metrics.items() if m.is_critical()]

    def mean_relative_error(self) -> float:
        """Rata-rata relative_error semua sinyal yang bisa dinilai."""
        vals = [
            m.relative_error
            for m in self.metrics.values()
            if m.flag != QualityFlag.ZERO_SIGNAL
        ]
        return float(np.mean(vals)) if vals else 0.0

    def mean_sparsity(self) -> float:
        vals = [
            m.sparsity_ratio
            for m in self.metrics.values()
            if m.flag != QualityFlag.ZERO_SIGNAL
        ]
        return float(np.mean(vals)) if vals else 0.0

    # ── Formatting ────────────────────────────────────────────────────────────

    def summary(self) -> str:
        """
        Satu baris ringkasan untuk console log.

        Contoh output:
          [Q] Win#5 Node1 | avg_err=0.0821 sparse=0.31 | OK
          [Q] Win#8 Node1 | avg_err=0.3102 sparse=0.45 | LOW_QUALITY: gx gz
        """
        avg_err   = self.mean_relative_error()
        avg_spar  = self.mean_sparsity()

        if self.has_critical():
            status = f"CRITICAL: {' '.join(self.critical_signals())}"
        elif self.has_low_quality():
            status = f"LOW_QUALITY: {' '.join(self.low_quality_signals())}"
        else:
            status = "OK"

        return (
            f"[Q] Win#{self.window_num} Node{self.node_id} | "
            f"avg_err={avg_err:.4f} sparse={avg_spar:.2f} | "
            f"{status}"
        )

    def detail_lines(self) -> list[str]:
        """Satu baris per sinyal untuk log verbose."""
        return [f"  {m.short_str()}" for m in self.metrics.values()]


# =============================================================================
# QualityAssessor
# =============================================================================

class QualityAssessor:
    """
    Hitung metrik kualitas rekonstruksi CS.

    Args:
        phi : np.ndarray (M × N) — measurement matrix Φ dari cs_router.PHI
              Dipakai untuk menghitung residual y - Φ·x̂.

    Contoh:
        from core.cs_router import PHI
        from core.quality   import QualityAssessor

        assessor = QualityAssessor(phi=PHI)
    """

    def __init__(self, phi: np.ndarray) -> None:
        if phi.ndim != 2:
            raise ValueError(f"PHI harus 2D, dapat shape {phi.shape}")
        self._phi = phi  # (M × N)

        # Counter statistik akumulatif
        self._total_windows:     int   = 0
        self._total_low_quality: int   = 0
        self._total_critical:    int   = 0
        self._sum_rel_error:     float = 0.0

    # ── Public API ────────────────────────────────────────────────────────────

    def assess_signal(
        self,
        signal: str,
        x_hat:  np.ndarray,
        y:      "list | np.ndarray",
    ) -> SignalMetric:
        """
        Hitung metrik kualitas untuk satu sinyal.

        Args:
            signal : nama sinyal, misalnya "ax" atau "ir"
            x_hat  : (N,) sinyal rekonstruksi dari OMP/LASSO
            y      : (M,) measurement vector asli dari sensor

        Returns:
            SignalMetric dengan semua field terisi
        """
        y_arr   = np.asarray(y,     dtype=np.float64)
        x_arr   = np.asarray(x_hat, dtype=np.float64)

        y_norm  = float(np.linalg.norm(y_arr))

        # Sinyal nol — tidak bisa dinilai
        if y_norm < ZERO_SIGNAL_THRESHOLD:
            return SignalMetric(
                signal         = signal,
                relative_error = 0.0,
                sparsity_ratio = 0.0,
                snr_db         = 0.0,
                flag           = QualityFlag.ZERO_SIGNAL,
                residual_norm  = 0.0,
                signal_norm    = y_norm,
            )

        # Residual: y - Φ·x̂
        y_hat         = self._phi @ x_arr          # (M,)
        residual      = y_arr - y_hat
        residual_norm = float(np.linalg.norm(residual))

        relative_error = residual_norm / y_norm

        # Sparsity: fraksi koefisien DCT yang dipakai OMP
        n_nonzero     = int(np.sum(np.abs(x_arr) > SPARSITY_EPSILON))
        sparsity_ratio = n_nonzero / max(len(x_arr), 1)

        # SNR estimasi
        if residual_norm > ZERO_SIGNAL_THRESHOLD:
            snr_db = 20.0 * math.log10(y_norm / residual_norm)
        else:
            snr_db = float("inf")

        # Flag
        if relative_error > THRESHOLD_CRITICAL:
            flag = QualityFlag.CRITICAL
        elif relative_error > THRESHOLD_LOW_QUALITY:
            flag = QualityFlag.LOW_QUALITY
        else:
            flag = QualityFlag.OK

        return SignalMetric(
            signal         = signal,
            relative_error = relative_error,
            sparsity_ratio = sparsity_ratio,
            snr_db         = snr_db,
            flag           = flag,
            residual_norm  = residual_norm,
            signal_norm    = y_norm,
        )

    def assess_window(
        self,
        results:      dict[str, np.ndarray],
        measurements: dict[str, "list | np.ndarray"],
        node_id:      int = 0,
        window_num:   int = 0,
    ) -> WindowReport:
        """
        Hitung metrik untuk semua sinyal dalam satu window sekaligus.

        Args:
            results      : {"ax": x_hat_ax, "gx": x_hat_gx, ...}
                           (output dari reconstruct() per sinyal)
            measurements : {"ax": y_ax, "gx": y_gx, ...}
                           (measurement vector asli dari payload MQTT)
            node_id      : untuk label di WindowReport
            window_num   : nomor window untuk label

        Returns:
            WindowReport berisi SignalMetric per sinyal
        """
        report = WindowReport(node_id=node_id, window_num=window_num)

        for sig, x_hat in results.items():
            y = measurements.get(sig)
            if y is None:
                continue
            report.metrics[sig] = self.assess_signal(sig, x_hat, y)

        # Update statistik internal
        self._total_windows += 1
        if report.has_critical():
            self._total_critical += 1
        elif report.has_low_quality():
            self._total_low_quality += 1
        self._sum_rel_error += report.mean_relative_error()

        return report

    # ── Statistik akumulatif ──────────────────────────────────────────────────

    def global_stats(self) -> dict:
        """
        Statistik akumulatif sejak assessor dibuat.
        Berguna untuk print ke console setiap N window.
        """
        n = max(self._total_windows, 1)
        return {
            "total_windows":     self._total_windows,
            "low_quality_count": self._total_low_quality,
            "critical_count":    self._total_critical,
            "ok_count":          self._total_windows
                                 - self._total_low_quality
                                 - self._total_critical,
            "low_quality_rate":  self._total_low_quality / n,
            "critical_rate":     self._total_critical / n,
            "avg_relative_error": self._sum_rel_error / n,
        }

    def stats_summary(self) -> str:
        """Satu baris ringkasan statistik global."""
        s = self.global_stats()
        return (
            f"[Q-Stats] "
            f"windows={s['total_windows']} | "
            f"ok={s['ok_count']} "
            f"low={s['low_quality_count']} "
            f"crit={s['critical_count']} | "
            f"avg_err={s['avg_relative_error']:.4f}"
        )
