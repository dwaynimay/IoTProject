"""
apps/ml_inference — Generic ML Inference Engine untuk Health Monitor.

Public API:

    # Single engine (satu model):
    from apps.ml_inference import MLInferenceEngine, WindowInput, InferenceResult

    # Multi-model registry (dinamis, banyak model parallel):
    from apps.ml_inference import ModelRegistry, MultiModelResult

    # Adapter dari pipeline / storage:
    from apps.ml_inference import from_processor, from_storage_rows

Cara pakai — single engine:
    engine = MLInferenceEngine()
    engine.load("models/activity_classifier.pkl", "models/activity_classifier_config.json")
    result = engine.predict(window)
    print(result.short_str())

Cara pakai — multi-model registry (direkomendasikan untuk produksi):
    registry = ModelRegistry()
    registry.scan("server/apps/ml_inference/models/")  # auto-detect semua .pkl + config

    # Atau register manual satu per satu:
    registry.register("models/fall_detector.pkl", "models/fall_detector_config.json")

    # Real-time per window:
    mmr = registry.predict_all(window)
    ws.send(json.dumps(mmr.to_dict()))   # langsung ke dashboard

    # Tambah model baru saat runtime (tanpa restart):
    registry.register("models/stress_monitor.pkl", "models/stress_monitor_config.json")

    # Status semua model (untuk REST /api/ml/status):
    print(registry.status())

Label tiap model sepenuhnya dikontrol dari _config.json masing-masing.
Tidak ada label yang hardcode di engine maupun registry.
"""

from .engine    import MLInferenceEngine
from .schemas   import WindowInput, InferenceResult, MultiModelResult
from .registry  import ModelRegistry
from .adapter   import from_processor, from_storage_rows
from .feature_extractor import FeatureExtractor
# from .wrappers  import SVMActivityWrapper

__all__ = [
    # Core engine
    "MLInferenceEngine",

    # Schemas (kontrak I/O)
    "WindowInput",
    "InferenceResult",
    "MultiModelResult",

    # Multi-model registry
    "ModelRegistry",

    # Feature extractor
    "FeatureExtractor",

    # Adapters
    "from_processor",
    "from_storage_rows",

    # Model wrappers
    # "SVMActivityWrapper",
]

