"""
feature_extractor.py — Ekstrak fitur dari WindowInput berdasarkan model_config.

Filosofi:
    Config mendefinisikan FITUR APA yang diekstrak dan DALAM URUTAN APA.
    Engine tidak hardcode fitur apapun — semua dikontrol dari JSON.

Format config "features" yang didukung:
    {
        "features": [
            {"name": "ax_mean",    "type": "stat",  "signal": "ax", "stat": "mean"},
            {"name": "ax_std",     "type": "stat",  "signal": "ax", "stat": "std"},
            {"name": "ax_min",     "type": "stat",  "signal": "ax", "stat": "min"},
            {"name": "ax_max",     "type": "stat",  "signal": "ax", "stat": "max"},
            {"name": "ax_rms",     "type": "stat",  "signal": "ax", "stat": "rms"},
            {"name": "ax_energy",  "type": "stat",  "signal": "ax", "stat": "energy"},
            {"name": "ax_range",   "type": "stat",  "signal": "ax", "stat": "range"},
            {"name": "ax_zcr",     "type": "stat",  "signal": "ax", "stat": "zcr"},
            {"name": "hr",         "type": "meta",  "field": "hr"},
            {"name": "spo2",       "type": "meta",  "field": "spo2",   "default": 0.0},
            {"name": "finger",     "type": "meta",  "field": "finger", "cast": "int"},
            {"name": "ax_raw_0",   "type": "raw",   "signal": "ax", "index": 0},
            ...
        ]
    }

Tipe fitur yang didukung:
    "stat"  → statistik dari sinyal rekonstruksi (64 float)
    "meta"  → field metadata (hr, spo2, finger, ts)
    "raw"   → satu elemen dari sinyal rekonstruksi (by index)
    "cross" → fitur cross-signal (contoh: corr antara ax dan ay)
"""

from __future__ import annotations

import logging
import math
from typing import Any

import numpy as np

from .schemas import WindowInput

logger = logging.getLogger(__name__)

# ── Stat functions yang didukung ──────────────────────────────────────────────

def _stat(arr: np.ndarray, stat_name: str) -> float:
    """Hitung satu statistik dari array."""
    if len(arr) == 0:
        return 0.0

    if stat_name == "mean":
        return float(np.mean(arr))
    elif stat_name == "std":
        return float(np.std(arr))
    elif stat_name == "min":
        return float(np.min(arr))
    elif stat_name == "max":
        return float(np.max(arr))
    elif stat_name == "rms":
        return float(np.sqrt(np.mean(arr ** 2)))
    elif stat_name == "energy":
        return float(np.sum(arr ** 2))
    elif stat_name == "range":
        return float(np.max(arr) - np.min(arr))
    elif stat_name == "zcr":
        # Zero-crossing rate
        signs = np.sign(arr)
        signs[signs == 0] = 1
        return float(np.sum(np.abs(np.diff(signs))) / (2 * len(arr)))
    elif stat_name == "median":
        return float(np.median(arr))
    elif stat_name in ("p25", "q1"):
        return float(np.percentile(arr, 25))
    elif stat_name in ("p75", "q3"):
        return float(np.percentile(arr, 75))
    elif stat_name == "skew":
        mu  = np.mean(arr)
        sig = np.std(arr)
        return float(np.mean(((arr - mu) / (sig + 1e-9)) ** 3))
    elif stat_name == "kurt":
        mu  = np.mean(arr)
        sig = np.std(arr)
        return float(np.mean(((arr - mu) / (sig + 1e-9)) ** 4) - 3)
    elif stat_name == "peak_freq":
        # Frekuensi dominan via FFT magnitude (dalam Hz, dengan FS=20Hz)
        fft_mag = np.abs(np.fft.rfft(arr))
        idx = np.argmax(fft_mag[1:]) + 1
        return float(idx * 20.0 / len(arr))
    elif stat_name == "spectral_energy":
        # Energi spektral ternormalisasi (dibagi panjang sinyal)
        fft_mag = np.abs(np.fft.rfft(arr))
        return float(np.sum(fft_mag ** 2) / len(arr))
    else:
        logger.warning("Stat '%s' tidak dikenal, return 0.0", stat_name)
        return 0.0


def _get_signal(window: WindowInput, signal_name: str) -> np.ndarray | None:
    """Ambil signal dari WindowInput sebagai ndarray, atau None jika tidak ada."""
    val = getattr(window, signal_name, None)
    if val is None:
        return None
    arr = np.asarray(val, dtype=np.float64)
    if len(arr) == 0:
        return None
    return arr


def _derived_signal(window: WindowInput, formula: str) -> np.ndarray | None:
    """
    Hitung sinyal derived dari formula sederhana.

    Formula yang didukung:
        "smv"  → sqrt(ax**2 + ay**2 + az**2)   Signal Magnitude Vector
        "smv_gyro" → sqrt(gx**2 + gy**2 + gz**2)

    Args:
        window  : WindowInput
        formula : nama formula (string)

    Returns:
        ndarray hasil komputasi, atau None jika sinyal sumber tidak tersedia.
    """
    if formula == "smv":
        ax = _get_signal(window, "ax")
        ay = _get_signal(window, "ay")
        az = _get_signal(window, "az")
        if ax is None or ay is None or az is None:
            return None
        n = min(len(ax), len(ay), len(az))
        return np.sqrt(ax[:n]**2 + ay[:n]**2 + az[:n]**2)

    elif formula == "smv_gyro":
        gx = _get_signal(window, "gx")
        gy = _get_signal(window, "gy")
        gz = _get_signal(window, "gz")
        if gx is None or gy is None or gz is None:
            return None
        n = min(len(gx), len(gy), len(gz))
        return np.sqrt(gx[:n]**2 + gy[:n]**2 + gz[:n]**2)

    else:
        logger.warning("Derived formula '%s' tidak dikenal", formula)
        return None


def _get_meta(window: WindowInput, field: str, default: Any, cast: str | None) -> float:
    """Ambil field metadata dari WindowInput."""
    val = getattr(window, field, None)
    if val is None:
        val = default
    if cast == "int":
        return float(int(bool(val)))
    if cast == "float" or cast is None:
        try:
            return float(val)
        except (TypeError, ValueError):
            return float(default) if default is not None else 0.0
    return float(default) if default is not None else 0.0


def _cross_feature(
    window: WindowInput,
    feat_def: dict,
) -> float:
    """Hitung fitur cross-signal."""
    cross_type = feat_def.get("cross_type", "corr")
    sig_a = _get_signal(window, feat_def["signal_a"])
    sig_b = _get_signal(window, feat_def["signal_b"])

    if sig_a is None or sig_b is None:
        return feat_def.get("default", 0.0)

    # Samakan panjang
    min_len = min(len(sig_a), len(sig_b))
    sig_a = sig_a[:min_len]
    sig_b = sig_b[:min_len]

    if cross_type == "corr":
        # Pearson correlation
        if np.std(sig_a) < 1e-9 or np.std(sig_b) < 1e-9:
            return 0.0
        return float(np.corrcoef(sig_a, sig_b)[0, 1])
    elif cross_type == "dot":
        return float(np.dot(sig_a, sig_b))
    elif cross_type == "diff_energy":
        return float(np.sum((sig_a - sig_b) ** 2))
    else:
        logger.warning("cross_type '%s' tidak dikenal, return 0.0", cross_type)
        return 0.0


# ── Public API ────────────────────────────────────────────────────────────────

class FeatureExtractor:
    """
    Ekstrak feature vector dari WindowInput berdasarkan schema di config.

    Args:
        feature_defs : list of dict dari model_config["features"]

    Contoh:
        extractor = FeatureExtractor(config["features"])
        vec, names, missing = extractor.extract(window_input)
    """

    def __init__(self, feature_defs: list[dict]) -> None:
        self._defs = feature_defs
        self._names = [f["name"] for f in feature_defs]
        logger.info("FeatureExtractor: %d fitur dikonfigurasi", len(self._defs))

    @property
    def feature_names(self) -> list[str]:
        return list(self._names)

    @property
    def n_features(self) -> int:
        return len(self._defs)

    def extract(
        self,
        window: WindowInput,
    ) -> tuple[list[float], list[str], list[str]]:
        """
        Ekstrak feature vector dari satu window.

        Returns:
            feature_vec : list float, panjang = n_features
            feature_names : nama tiap fitur (urutan sama)
            missing     : list nama fitur yang fallback ke default
                          (berguna untuk debug / QA)
        """
        vec: list[float] = []
        missing: list[str] = []

        for feat in self._defs:
            feat_name = feat["name"]
            feat_type = feat.get("type", "stat")
            default   = feat.get("default", 0.0)

            try:
                if feat_type == "stat":
                    signal_name = feat["signal"]
                    stat_name   = feat["stat"]
                    arr = _get_signal(window, signal_name)
                    if arr is None:
                        vec.append(float(default))
                        missing.append(feat_name)
                    else:
                        vec.append(_stat(arr, stat_name))

                elif feat_type == "meta":
                    field_name = feat["field"]
                    cast       = feat.get("cast", None)
                    val = _get_meta(window, field_name, default, cast)
                    vec.append(val)

                elif feat_type == "raw":
                    signal_name = feat["signal"]
                    index       = int(feat["index"])
                    arr = _get_signal(window, signal_name)
                    if arr is None or index >= len(arr):
                        vec.append(float(default))
                        missing.append(feat_name)
                    else:
                        vec.append(float(arr[index]))

                elif feat_type == "cross":
                    val = _cross_feature(window, feat)
                    vec.append(val)

                elif feat_type == "derived":
                    formula     = feat["formula"]   # e.g. "smv"
                    stat_name   = feat["stat"]       # e.g. "mean"
                    arr = _derived_signal(window, formula)
                    if arr is None:
                        vec.append(float(default))
                        missing.append(feat_name)
                    else:
                        vec.append(_stat(arr, stat_name))

                else:
                    logger.warning("feat type '%s' tidak dikenal ('%s'), pakai default",
                                   feat_type, feat_name)
                    vec.append(float(default))
                    missing.append(feat_name)

            except Exception as exc:
                logger.error("Error ekstrak fitur '%s': %s", feat_name, exc)
                vec.append(float(default))
                missing.append(feat_name)

        return vec, self._names, missing
