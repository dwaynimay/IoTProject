"""
engine.py — Generic ML Inference Engine.

Filosofi (mirip SnortML):
    Engine tidak tahu dan tidak peduli bagaimana model di-train.
    Yang penting:
      1. Model file: .pkl (sklearn Pipeline / estimator / anything with predict_proba)
      2. Manifest:   model_config.json (feature schema, labels, preprocessing flags)

    Ganti model → ganti .pkl + .json → reload → selesai.
    Tidak perlu ubah satu baris kode engine.

Kontrak model (.pkl):
    - Harus punya method predict_proba(X) → ndarray (n_samples, n_classes)
    - X adalah ndarray shape (1, n_features), tipe float64
    - Bisa berupa sklearn Pipeline, estimator tunggal, atau wrapper apapun
      selama memenuhi interface di atas

Kontrak model_config.json:
    {
        "model_name": "activity_classifier_v1",
        "model_version": "1.0.0",
        "description": "...",

        "labels": ["normal", "tachycardia", "bradycardia", "low_spo2"],

        "features": [                          ← schema fitur, lihat feature_extractor.py
            {"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"},
            ...
        ],

        "skip_if": {                           ← kondisi skip window (opsional)
            "finger_required": true,           ← skip jika finger=false
            "min_hr": 20,                      ← skip jika hr < 20
            "require_signals": ["ax", "ir"]    ← skip jika sinyal ini None
        },

        "output": {
            "confidence_threshold": 0.0        ← bawah ini → label = "uncertain"
        }
    }
"""

from __future__ import annotations

import json
import logging
import pickle
import time
from pathlib import Path
from typing import Any, Optional

import numpy as np

from .feature_extractor import FeatureExtractor
from .schemas import InferenceResult, WindowInput

logger = logging.getLogger(__name__)


# ── Engine ────────────────────────────────────────────────────────────────────

class MLInferenceEngine:
    """
    Generic inference engine untuk model .pkl + manifest model_config.json.

    Lifecycle:
        engine = MLInferenceEngine()
        engine.load(model_path="models/v1.pkl", config_path="models/v1_config.json")

        # Real-time (satu window):
        result = engine.predict(window_input)

        # Batch (dari SQLite atau list):
        results = engine.predict_batch(window_inputs)

        # Reload model tanpa restart:
        engine.load(...)
    """

    def __init__(self) -> None:
        self._model:     Any = None
        self._config:    dict = {}
        self._extractor: Optional[FeatureExtractor] = None
        self._labels:    list[str] = []
        self._model_name:    str = "none"
        self._model_version: str = "0"
        self._loaded:    bool = False

        # Statistik
        self._total_inferred: int   = 0
        self._total_skipped:  int   = 0
        self._total_ms:       float = 0.0

    # ── Load ─────────────────────────────────────────────────────────────────

    def load(
        self,
        model_path:  "str | Path",
        config_path: "str | Path",
    ) -> None:
        """
        Load model .pkl dan manifest JSON.

        Thread-safe: assign atomik di akhir sehingga predict() yang sedang
        berjalan tidak terganggu (GIL Python menjamin assign reference atomik).

        Args:
            model_path  : path ke file .pkl
            config_path : path ke model_config.json
        """
        model_path  = Path(model_path)
        config_path = Path(config_path)

        if not model_path.exists():
            raise FileNotFoundError(f"Model tidak ditemukan: {model_path}")
        if not config_path.exists():
            raise FileNotFoundError(f"Config tidak ditemukan: {config_path}")

        # Load config dulu — validasi sebelum load model yang mungkin besar
        with open(config_path, encoding="utf-8") as f:
            config = json.load(f)

        _validate_config(config)

        # Load model
        with open(model_path, "rb") as f:
            model = pickle.load(f)

        _validate_model(model)
        _validate_model_labels(model, config["labels"])

        # Build extractor dari schema fitur di config
        extractor = FeatureExtractor(config["features"])

        # Verifikasi n_features match jika model punya attr tersebut
        if hasattr(model, "n_features_in_"):
            expected = model.n_features_in_
            got      = extractor.n_features
            if expected != got:
                raise ValueError(
                    f"Jumlah fitur tidak match: model expect {expected}, "
                    f"config define {got}"
                )

        # Atomik assign — thread safety via GIL
        self._model      = model
        self._config     = config
        self._extractor  = extractor
        self._labels     = config["labels"]
        self._model_name    = config.get("model_name", model_path.stem)
        self._model_version = config.get("model_version", "?")
        self._loaded     = True

        logger.info(
            "Model loaded: %s v%s | features=%d | labels=%s",
            self._model_name, self._model_version,
            extractor.n_features, self._labels,
        )

    def unload(self) -> None:
        """Hapus model dari memori."""
        self._model     = None
        self._extractor = None
        self._loaded    = False
        logger.info("Model unloaded.")

    # ── Predict — single window ───────────────────────────────────────────────

    def predict(self, window: WindowInput) -> InferenceResult:
        """
        Infer satu window. Thread-safe (read-only akses ke model).

        Returns InferenceResult dengan skipped=True jika:
          - Engine belum di-load
          - Kondisi skip_if di config terpenuhi
          - Exception saat ekstraksi / inferensi
        """
        if not self._loaded:
            return InferenceResult(
                node_id=window.node_id, window_num=window.window_num, ts=window.ts,
                skipped=True, skip_reason="engine not loaded",
            )

        # Cek kondisi skip
        skip_reason = self._check_skip(window)
        if skip_reason:
            self._total_skipped += 1
            return InferenceResult(
                node_id=window.node_id, window_num=window.window_num, ts=window.ts,
                skipped=True, skip_reason=skip_reason,
            )

        t0 = time.perf_counter()

        try:
            # Ekstrak fitur
            feat_vec, feat_names, missing = self._extractor.extract(window)

            if missing:
                logger.debug("Win %d: %d fitur fallback ke default: %s",
                             window.window_num, len(missing), missing[:5])

            X = np.array(feat_vec, dtype=np.float64).reshape(1, -1)

            # Inferensi
            proba_arr = self._model.predict_proba(X)[0]  # shape (n_classes,)
            if len(proba_arr) != len(self._labels):
                raise ValueError(
                    f"Model menghasilkan {len(proba_arr)} probabilitas, "
                    f"manifest mendefinisikan {len(self._labels)} label"
                )

            # Map ke dict label → proba
            proba_dict = {
                label: float(proba_arr[i])
                for i, label in enumerate(self._labels)
                if i < len(proba_arr)
            }

            # Label terbaik
            best_label = max(proba_dict, key=proba_dict.get)
            confidence = proba_dict[best_label]

            # Threshold: jika confidence terlalu rendah → "uncertain"
            threshold = self._config.get("output", {}).get("confidence_threshold", 0.0)
            if confidence < threshold:
                best_label = "uncertain"

            elapsed_ms = (time.perf_counter() - t0) * 1000
            self._total_inferred += 1
            self._total_ms       += elapsed_ms

            return InferenceResult(
                node_id    = window.node_id,
                window_num = window.window_num,
                ts         = window.ts,
                label      = best_label,
                confidence = confidence,
                proba      = proba_dict,
                feature_vec = feat_vec,
            )

        except Exception as exc:
            logger.error("Inference error win %d: %s", window.window_num, exc, exc_info=True)
            self._total_skipped += 1
            return InferenceResult(
                node_id=window.node_id, window_num=window.window_num, ts=window.ts,
                skipped=True, skip_reason=f"inference error: {exc}",
            )

    # ── Predict — batch ───────────────────────────────────────────────────────

    def predict_batch(
        self,
        windows: list[WindowInput],
        *,
        skip_errors: bool = True,
    ) -> list[InferenceResult]:
        """
        Infer batch window. Lebih efisien untuk data SQLite.

        Jika model mendukung predict_proba pada batch, dilakukan sekaligus.
        Fallback ke per-window jika ada error.

        Args:
            windows     : list WindowInput
            skip_errors : jika True, window error di-skip (tidak raise)

        Returns:
            list InferenceResult, panjang sama dengan windows
        """
        if not self._loaded:
            return [
                InferenceResult(
                    node_id=w.node_id, window_num=w.window_num, ts=w.ts,
                    skipped=True, skip_reason="engine not loaded",
                )
                for w in windows
            ]

        # Pisahkan yang skip dan yang valid
        results: list[InferenceResult] = [None] * len(windows)  # type: ignore
        valid_idx:   list[int]         = []
        valid_feats: list[list[float]] = []

        for i, win in enumerate(windows):
            skip_reason = self._check_skip(win)
            if skip_reason:
                self._total_skipped += 1
                results[i] = InferenceResult(
                    node_id=win.node_id, window_num=win.window_num, ts=win.ts,
                    skipped=True, skip_reason=skip_reason,
                )
                continue

            try:
                feat_vec, _, missing = self._extractor.extract(win)
                valid_idx.append(i)
                valid_feats.append(feat_vec)
            except Exception as exc:
                self._total_skipped += 1
                results[i] = InferenceResult(
                    node_id=win.node_id, window_num=win.window_num, ts=win.ts,
                    skipped=True, skip_reason=f"feature extraction error: {exc}",
                )

        if not valid_feats:
            return results

        # Batch predict
        t0 = time.perf_counter()
        try:
            X         = np.array(valid_feats, dtype=np.float64)
            proba_mat = self._model.predict_proba(X)   # (n_valid, n_classes)
            if proba_mat.ndim != 2 or proba_mat.shape[1] != len(self._labels):
                raise ValueError(
                    f"Shape probabilitas {proba_mat.shape} tidak cocok dengan "
                    f"{len(self._labels)} label"
                )
            elapsed_ms = (time.perf_counter() - t0) * 1000
            self._total_ms += elapsed_ms

            threshold = self._config.get("output", {}).get("confidence_threshold", 0.0)

            for batch_i, orig_i in enumerate(valid_idx):
                win       = windows[orig_i]
                proba_arr = proba_mat[batch_i]
                proba_dict = {
                    label: float(proba_arr[j])
                    for j, label in enumerate(self._labels)
                    if j < len(proba_arr)
                }
                best_label = max(proba_dict, key=proba_dict.get)
                confidence = proba_dict[best_label]

                if confidence < threshold:
                    best_label = "uncertain"

                results[orig_i] = InferenceResult(
                    node_id    = win.node_id,
                    window_num = win.window_num,
                    ts         = win.ts,
                    label      = best_label,
                    confidence = confidence,
                    proba      = proba_dict,
                    feature_vec = valid_feats[batch_i],
                )
                self._total_inferred += 1

        except Exception as exc:
            logger.error("Batch inference error: %s — fallback ke per-window", exc)
            if not skip_errors:
                raise
            # Fallback per-window untuk yang valid
            for batch_i, orig_i in enumerate(valid_idx):
                if results[orig_i] is None:
                    results[orig_i] = self.predict(windows[orig_i])

        return results

    # ── Status ────────────────────────────────────────────────────────────────

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    @property
    def model_name(self) -> str:
        return self._model_name

    @property
    def model_version(self) -> str:
        return self._model_version

    @property
    def labels(self) -> list[str]:
        return list(self._labels)

    def stats(self) -> dict:
        total = self._total_inferred + self._total_skipped
        avg_ms = (self._total_ms / self._total_inferred
                  if self._total_inferred > 0 else 0.0)
        return {
            "model_name":    self._model_name,
            "model_version": self._model_version,
            "is_loaded":     self._loaded,
            "total_inferred": self._total_inferred,
            "total_skipped":  self._total_skipped,
            "skip_rate":      self._total_skipped / max(total, 1),
            "avg_infer_ms":   round(avg_ms, 3),
            "labels":         self._labels,
            "n_features":     self._extractor.n_features if self._extractor else 0,
        }

    def stats_summary(self) -> str:
        s = self.stats()
        return (
            f"[ML] {s['model_name']} v{s['model_version']} | "
            f"inferred={s['total_inferred']} skip={s['total_skipped']} "
            f"({s['skip_rate']:.1%}) | avg={s['avg_infer_ms']}ms"
        )

    # ── Internal ─────────────────────────────────────────────────────────────

    def _check_skip(self, window: WindowInput) -> str:
        """
        Cek apakah window harus di-skip berdasarkan config skip_if.
        Return string alasan jika skip, kosong jika tidak.
        """
        skip_cfg = self._config.get("skip_if", {})
        if not skip_cfg:
            return ""

        if skip_cfg.get("finger_required", False) and not window.finger:
            return "finger not detected"

        min_hr = skip_cfg.get("min_hr")
        if min_hr is not None and window.hr < min_hr:
            return f"hr={window.hr} < min_hr={min_hr}"

        max_hr = skip_cfg.get("max_hr")
        if max_hr is not None and window.hr > max_hr:
            return f"hr={window.hr} > max_hr={max_hr}"

        required_signals = skip_cfg.get("require_signals", [])
        for sig in required_signals:
            if getattr(window, sig, None) is None:
                return f"required signal '{sig}' is None"

        return ""


# ── Validation helpers ────────────────────────────────────────────────────────

def _validate_config(config: dict) -> None:
    """Validasi minimal struktur model_config.json."""
    required_keys = ("labels", "features")
    missing = [k for k in required_keys if k not in config]
    if missing:
        raise ValueError(f"model_config.json missing keys: {missing}")

    if not isinstance(config["labels"], list) or len(config["labels"]) < 2:
        raise ValueError("config['labels'] harus list dengan minimal 2 kelas")
    if any(not isinstance(label, str) or not label for label in config["labels"]):
        raise ValueError("semua config['labels'] harus string non-kosong")
    if len(set(config["labels"])) != len(config["labels"]):
        raise ValueError("config['labels'] tidak boleh duplikat")

    if not isinstance(config["features"], list) or len(config["features"]) == 0:
        raise ValueError("config['features'] harus list non-kosong")

    for i, feat in enumerate(config["features"]):
        if "name" not in feat:
            raise ValueError(f"feature[{i}] missing 'name'")
        if "type" not in feat:
            raise ValueError(f"feature[{i}] ('{feat['name']}') missing 'type'")


def _validate_model(model: Any) -> None:
    """Validasi model punya interface yang dibutuhkan."""
    if not hasattr(model, "predict_proba"):
        raise TypeError(
            f"Model {type(model).__name__} tidak punya method predict_proba(). "
            "Wrapper model dengan predict_proba jika perlu."
        )


def _validate_model_labels(model: Any, labels: list[str]) -> None:
    """Reject manifest/model class mappings that can be proven inconsistent."""
    if not hasattr(model, "classes_"):
        return

    classes = list(model.classes_)
    if len(classes) != len(labels):
        raise ValueError(
            f"Model memiliki {len(classes)} kelas, manifest memiliki {len(labels)} label"
        )
    if all(isinstance(value, str) for value in classes) and classes != labels:
        raise ValueError(
            f"Urutan classes_ model {classes} tidak sama dengan labels manifest {labels}"
        )
