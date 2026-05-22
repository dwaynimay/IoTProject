"""
core/inference.py — Interface untuk ML inference engine.

Modul ini menyediakan abstraksi layer untuk integrasi model Machine Learning
ke dalam pipeline rekonstruksi dan monitoring kesehatan.

Arsitektur:
    Data MQTT → Validate → Reconstruct → Quality Assess → **ML Inference** → Dashboard

Saat ini berisi interface dan placeholder. Implementasi aktual akan ditambahkan
setelah model ML selesai di-train.

Contoh penggunaan (setelah model ready):
    from apps.ml_inference.inference import InferenceEngine

    engine = InferenceEngine()
    engine.load_model("models/health_classifier_v1.onnx")

    prediction = engine.predict({
        "hr": 72,
        "spo2": 98.1,
        "imu_features": {...},
        "quality_score": 0.92,
    })
"""

from __future__ import annotations

import logging
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Optional

import numpy as np

logger = logging.getLogger(__name__)


# ── Data classes ──────────────────────────────────────────────────────────────

@dataclass
class PredictionResult:
    """Hasil prediksi dari ML model."""
    label:      str                      # e.g. "normal", "abnormal", "tachycardia"
    confidence: float                    # 0.0 – 1.0
    details:    dict[str, Any] = field(default_factory=dict)


@dataclass
class FeatureVector:
    """Fitur yang diekstrak dari window rekonstruksi untuk ML inference."""
    node_id:       int
    window_num:    int
    hr:            int            = -1
    spo2:          Optional[float] = None
    finger:        bool           = False
    imu_stats:     dict[str, dict[str, float]] = field(default_factory=dict)
    quality_score: float          = 0.0
    timestamp_ms:  int            = 0


# ── Abstract base ─────────────────────────────────────────────────────────────

class BaseInferenceEngine(ABC):
    """
    Abstract base class untuk ML inference.

    Subclass harus mengimplementasikan:
      - load_model()   : load model dari file/path
      - _extract()     : ekstrak fitur dari data window
      - _infer()       : jalankan inferensi
    """

    @abstractmethod
    def load_model(self, model_path: str) -> None:
        """Load ML model dari disk."""
        ...

    @abstractmethod
    def _extract_features(self, window_data: dict) -> FeatureVector:
        """Ekstrak fitur dari data window rekonstruksi."""
        ...

    @abstractmethod
    def _infer(self, features: FeatureVector) -> PredictionResult:
        """Jalankan inferensi pada fitur yang sudah diekstrak."""
        ...

    def predict(self, window_data: dict) -> PredictionResult:
        """
        Pipeline lengkap: extract features → infer → return result.

        Args:
            window_data: dict berisi hasil rekonstruksi, metadata sensor,
                         dan quality report dari satu window.

        Returns:
            PredictionResult dengan label, confidence, dan detail.
        """
        features = self._extract_features(window_data)
        result   = self._infer(features)
        logger.debug(
            "Inference: node=%d win=%d → %s (%.2f)",
            features.node_id, features.window_num,
            result.label, result.confidence,
        )
        return result

    @property
    @abstractmethod
    def is_loaded(self) -> bool:
        """True jika model sudah di-load dan siap inferensi."""
        ...


# ── Placeholder implementation ────────────────────────────────────────────────

class InferenceEngine(BaseInferenceEngine):
    """
    Placeholder ML engine.

    Mengembalikan prediksi rule-based sederhana berdasarkan HR dan SpO2.
    Akan diganti dengan model ONNX/TFLite setelah training selesai.
    """

    def __init__(self) -> None:
        self._model_loaded = False
        self._model_path:  Optional[str] = None

    def load_model(self, model_path: str) -> None:
        """
        Placeholder: akan load model ONNX/TFLite di sini.

        TODO: Implementasi aktual setelah model selesai training.
              Contoh: onnxruntime.InferenceSession(model_path)
        """
        self._model_path   = model_path
        self._model_loaded = True
        logger.info("Model loaded (placeholder): %s", model_path)

    def _extract_features(self, window_data: dict) -> FeatureVector:
        """Ekstrak fitur dasar dari data window."""
        results = window_data.get("results", {})

        imu_stats: dict[str, dict[str, float]] = {}
        for sig, values in results.items():
            if isinstance(values, np.ndarray):
                imu_stats[sig] = {
                    "mean": float(np.mean(values)),
                    "std":  float(np.std(values)),
                    "min":  float(np.min(values)),
                    "max":  float(np.max(values)),
                }

        report = window_data.get("report")
        quality_score = 0.0
        if report is not None and hasattr(report, "mean_relative_error"):
            quality_score = max(0.0, 1.0 - report.mean_relative_error())

        return FeatureVector(
            node_id       = window_data.get("node_id", 0),
            window_num    = window_data.get("window_num", 0),
            hr            = window_data.get("hr", -1),
            spo2          = window_data.get("spo2"),
            finger        = window_data.get("finger", False),
            imu_stats     = imu_stats,
            quality_score = quality_score,
            timestamp_ms  = window_data.get("ts", 0),
        )

    def _infer(self, features: FeatureVector) -> PredictionResult:
        """
        Rule-based placeholder inference.

        TODO: Ganti dengan model inference aktual:
              session = onnxruntime.InferenceSession(self._model_path)
              output  = session.run(None, {input_name: feature_array})
        """
        hr   = features.hr
        spo2 = features.spo2

        # Rule-based classification (placeholder)
        if not features.finger or hr <= 0:
            return PredictionResult(
                label="no_data",
                confidence=0.0,
                details={"reason": "Jari tidak terdeteksi atau HR invalid"},
            )

        if hr > 100:
            label      = "tachycardia"
            confidence = min((hr - 100) / 40, 1.0)
        elif hr < 60:
            label      = "bradycardia"
            confidence = min((60 - hr) / 20, 1.0)
        elif spo2 is not None and spo2 < 95.0:
            label      = "low_spo2"
            confidence = min((95.0 - spo2) / 10, 1.0)
        else:
            label      = "normal"
            confidence = features.quality_score

        return PredictionResult(
            label      = label,
            confidence = round(confidence, 3),
            details    = {
                "hr":            hr,
                "spo2":          spo2,
                "quality_score": round(features.quality_score, 3),
            },
        )

    @property
    def is_loaded(self) -> bool:
        return self._model_loaded
