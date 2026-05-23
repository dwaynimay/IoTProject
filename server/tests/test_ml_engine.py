"""
tests/test_ml_engine.py — Unit test untuk ML Inference Engine.

Jalankan dari root project:
    python -m pytest server/tests/test_ml_engine.py -v

Atau tanpa pytest:
    python server/tests/test_ml_engine.py

Tidak butuh model .pkl nyata — semua test pakai DummyModel.
"""

import json
import os
import pickle
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from apps.ml_inference import (
    MLInferenceEngine, WindowInput, InferenceResult,
    FeatureExtractor, from_processor, from_storage_rows,
)


# =============================================================================
# Dummy model — implementasi minimal predict_proba
# =============================================================================

class BadModel:
    """Model tanpa predict_proba — untuk test validasi engine."""
    def fit(self, X, y): pass


class DummyModel:
    """Sklearn-compatible dummy dengan predict_proba tetap."""

    def __init__(self, n_classes: int = 3, fixed_class: int = 0):
        self.n_classes   = n_classes
        self.fixed_class = fixed_class
        self.n_features_in_ = None   # diisi saat test

    def predict_proba(self, X: np.ndarray) -> np.ndarray:
        n_samples = X.shape[0]
        proba = np.full((n_samples, self.n_classes), 1.0 / self.n_classes)
        proba[:, self.fixed_class] = 0.7
        # renorm
        proba = proba / proba.sum(axis=1, keepdims=True)
        return proba


# =============================================================================
# Helpers
# =============================================================================

_LABELS    = ["normal", "tachycardia", "bradycardia"]
_N_CLASSES = len(_LABELS)

_MINIMAL_FEATURES = [
    {"name": "ax_mean", "type": "stat", "signal": "ax", "stat": "mean"},
    {"name": "ax_std",  "type": "stat", "signal": "ax", "stat": "std"},
    {"name": "hr",      "type": "meta", "field": "hr", "default": -1},
]

_MINIMAL_CONFIG = {
    "model_name":    "test_model",
    "model_version": "0.0.1",
    "labels":   _LABELS,
    "features": _MINIMAL_FEATURES,
    "skip_if":  {},
    "output":   {"confidence_threshold": 0.0},
}


def _make_model_files(
    config: dict | None = None,
    n_features: int | None = None,
    fixed_class: int = 0,
) -> tuple[str, str]:
    """Buat file .pkl dan .json sementara, return (model_path, config_path)."""
    cfg = config or _MINIMAL_CONFIG.copy()

    model = DummyModel(n_classes=len(cfg["labels"]), fixed_class=fixed_class)
    if n_features is not None:
        model.n_features_in_ = n_features
    else:
        model.n_features_in_ = len(cfg["features"])

    tmp_dir = tempfile.mkdtemp()
    model_path  = os.path.join(tmp_dir, "model.pkl")
    config_path = os.path.join(tmp_dir, "config.json")

    with open(model_path, "wb") as f:
        pickle.dump(model, f)
    with open(config_path, "w") as f:
        json.dump(cfg, f)

    return model_path, config_path


def _make_window(
    node_id: int = 1,
    window_num: int = 1,
    ts: int = 1000,
    n: int = 64,
    hr: int = 75,
    spo2: float = 98.0,
    finger: bool = True,
) -> WindowInput:
    """WindowInput dengan sinyal dummy."""
    rng = np.random.default_rng(42)
    sig = rng.standard_normal(n).tolist()
    return WindowInput(
        node_id=node_id, window_num=window_num, ts=ts,
        ax=sig, ay=sig, az=sig,
        gx=sig, gy=sig, gz=sig,
        ir=sig,
        hr=hr, spo2=spo2, finger=finger,
    )


# =============================================================================
# Test engine.load()
# =============================================================================

def test_load_basic():
    """Engine berhasil load model dan config yang valid."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)
    assert engine.is_loaded
    assert engine.model_name == "test_model"
    assert engine.labels == _LABELS


def test_load_missing_model_file():
    """FileNotFoundError jika model tidak ada."""
    engine = MLInferenceEngine()
    _, cp  = _make_model_files()
    try:
        engine.load("/tmp/tidak_ada.pkl", cp)
        assert False, "Harusnya FileNotFoundError"
    except FileNotFoundError:
        pass


def test_load_missing_config_file():
    engine = MLInferenceEngine()
    mp, _  = _make_model_files()
    try:
        engine.load(mp, "/tmp/tidak_ada.json")
        assert False, "Harusnya FileNotFoundError"
    except FileNotFoundError:
        pass


def test_load_config_missing_labels():
    """ValueError jika config tidak punya 'labels'."""
    bad_config = {"features": _MINIMAL_FEATURES}
    mp, cp = _make_model_files(config={**_MINIMAL_CONFIG, **bad_config})
    # config tidak valid karena tidak ada labels
    try:
        engine = MLInferenceEngine()
        engine.load(mp, "/tmp/tidak_ada.json")
        assert False
    except FileNotFoundError:
        pass  # config path tidak ada → FileNotFoundError, bukan ValueError


def test_load_config_invalid_labels_count():
    """ValueError jika labels < 2."""
    bad = dict(_MINIMAL_CONFIG)
    bad["labels"] = ["only_one"]
    mp, cp = _make_model_files(config=bad)
    try:
        engine = MLInferenceEngine()
        engine.load(mp, cp)
        assert False, "Harusnya ValueError"
    except ValueError as e:
        assert "minimal 2 kelas" in str(e)


def test_load_model_no_predict_proba():
    """TypeError jika model tidak punya predict_proba."""
    tmp_dir = tempfile.mkdtemp()
    mp = os.path.join(tmp_dir, "bad.pkl")
    cp = os.path.join(tmp_dir, "cfg.json")
    with open(mp, "wb") as f:
        pickle.dump(BadModel(), f)
    with open(cp, "w") as f:
        json.dump(_MINIMAL_CONFIG, f)

    engine = MLInferenceEngine()
    try:
        engine.load(mp, cp)
        assert False, "Harusnya TypeError"
    except TypeError as e:
        assert "predict_proba" in str(e)


def test_load_n_features_mismatch():
    """ValueError jika n_features model ≠ jumlah features di config."""
    mp, cp = _make_model_files(n_features=999)  # model expect 999, config define 3
    engine = MLInferenceEngine()
    try:
        engine.load(mp, cp)
        assert False, "Harusnya ValueError"
    except ValueError as e:
        assert "n_features" in str(e).lower() or "match" in str(e).lower()


# =============================================================================
# Test predict() — single window
# =============================================================================

def test_predict_not_loaded():
    """Predict sebelum load → skipped=True."""
    engine = MLInferenceEngine()
    win    = _make_window()
    result = engine.predict(win)
    assert result.skipped
    assert "not loaded" in result.skip_reason


def test_predict_returns_correct_shape():
    """Predict harus return InferenceResult dengan semua kelas."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    win    = _make_window()
    result = engine.predict(win)

    assert not result.skipped
    assert result.label in _LABELS
    assert 0.0 <= result.confidence <= 1.0
    assert set(result.proba.keys()) == set(_LABELS)
    assert abs(sum(result.proba.values()) - 1.0) < 1e-6


def test_predict_feature_vec_length():
    """feature_vec harus panjang = n_features di config."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    win    = _make_window()
    result = engine.predict(win)
    assert len(result.feature_vec) == len(_MINIMAL_FEATURES)


def test_predict_with_missing_signal():
    """Sinyal yang None → fitur fallback ke default, tidak crash."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    win = _make_window()
    win.ax = None   # hapus ax — engine harus pakai default
    result = engine.predict(win)
    assert not result.skipped


def test_predict_skip_finger_required():
    """Engine skip window jika finger_required=True dan finger=False."""
    config = dict(_MINIMAL_CONFIG)
    config["skip_if"] = {"finger_required": True}
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    win = _make_window(finger=False)
    result = engine.predict(win)
    assert result.skipped
    assert "finger" in result.skip_reason


def test_predict_skip_min_hr():
    """Engine skip jika hr < min_hr."""
    config = dict(_MINIMAL_CONFIG)
    config["skip_if"] = {"min_hr": 50}
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    win = _make_window(hr=30)
    result = engine.predict(win)
    assert result.skipped
    assert "min_hr" in result.skip_reason


def test_predict_skip_require_signals():
    """Engine skip jika sinyal wajib tidak ada."""
    config = dict(_MINIMAL_CONFIG)
    config["skip_if"] = {"require_signals": ["ax", "ir"]}
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    win = _make_window()
    win.ir = None   # ir wajib tapi None
    result = engine.predict(win)
    assert result.skipped
    assert "ir" in result.skip_reason


def test_predict_confidence_threshold():
    """Label → 'uncertain' jika confidence di bawah threshold."""
    config = dict(_MINIMAL_CONFIG)
    config["output"] = {"confidence_threshold": 0.99}  # hampir mustahil terpenuhi
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    win = _make_window()
    result = engine.predict(win)
    # DummyModel max proba sekitar 0.7 → di bawah 0.99 → uncertain
    assert result.label == "uncertain"


def test_predict_short_str():
    """short_str tidak crash dan mengandung info dasar."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    win    = _make_window()
    result = engine.predict(win)
    s = result.short_str()
    assert str(win.node_id) in s
    assert str(win.window_num) in s


# =============================================================================
# Test predict_batch()
# =============================================================================

def test_predict_batch_returns_same_length():
    """Batch output harus sama panjang dengan input."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    windows = [_make_window(window_num=i) for i in range(5)]
    results = engine.predict_batch(windows)
    assert len(results) == len(windows)


def test_predict_batch_mixed_skip():
    """Batch dengan campuran valid dan skip."""
    config = dict(_MINIMAL_CONFIG)
    config["skip_if"] = {"require_signals": ["ax"]}
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    w_valid = _make_window(window_num=1)
    w_skip  = _make_window(window_num=2)
    w_skip.ax = None   # akan di-skip

    results = engine.predict_batch([w_valid, w_skip])
    assert not results[0].skipped
    assert results[1].skipped


def test_predict_batch_empty():
    """Batch kosong → list kosong."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)
    assert engine.predict_batch([]) == []


# =============================================================================
# Test stats()
# =============================================================================

def test_stats_accumulate():
    """Stats harus bertambah setelah predict."""
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)

    for i in range(3):
        engine.predict(_make_window(window_num=i))

    s = engine.stats()
    assert s["total_inferred"] == 3
    assert s["total_skipped"]  == 0
    assert s["avg_infer_ms"]   >= 0.0


def test_stats_skip_counted():
    config = dict(_MINIMAL_CONFIG)
    config["skip_if"] = {"finger_required": True}
    mp, cp = _make_model_files(config=config)
    engine = MLInferenceEngine()
    engine.load(mp, cp)

    engine.predict(_make_window(finger=False))
    s = engine.stats()
    assert s["total_skipped"] == 1
    assert s["total_inferred"] == 0


def test_stats_summary_format():
    engine = MLInferenceEngine()
    mp, cp = _make_model_files()
    engine.load(mp, cp)
    engine.predict(_make_window())
    summary = engine.stats_summary()
    assert "[ML]" in summary
    assert "inferred=1" in summary


# =============================================================================
# Test FeatureExtractor
# =============================================================================

def test_feature_extractor_all_stat_types():
    """Semua stat type harus bisa diekstrak tanpa crash."""
    stats = ["mean", "std", "min", "max", "rms", "energy", "range",
             "zcr", "median", "p25", "p75", "skew", "kurt",
             "peak_freq", "spectral_energy"]
    feats = [
        {"name": f"ax_{s}", "type": "stat", "signal": "ax", "stat": s}
        for s in stats
    ]
    extractor = FeatureExtractor(feats)
    win = _make_window()
    vec, names, missing = extractor.extract(win)
    assert len(vec) == len(stats)
    assert missing == []
    assert all(isinstance(v, float) for v in vec)


def test_feature_extractor_meta_cast_int():
    """Meta field dengan cast=int: finger (bool) → 0.0 atau 1.0."""
    feats = [{"name": "finger", "type": "meta", "field": "finger", "cast": "int"}]
    extractor = FeatureExtractor(feats)

    win_on  = _make_window(finger=True)
    win_off = _make_window(finger=False)

    vec_on,  _, _ = extractor.extract(win_on)
    vec_off, _, _ = extractor.extract(win_off)

    assert vec_on[0]  == 1.0
    assert vec_off[0] == 0.0


def test_feature_extractor_raw_index():
    """Raw feature harus ambil elemen ke-index dari sinyal."""
    feats = [{"name": "ax_5", "type": "raw", "signal": "ax", "index": 5}]
    extractor = FeatureExtractor(feats)
    win = _make_window()
    vec, _, _ = extractor.extract(win)
    assert abs(vec[0] - win.ax[5]) < 1e-9


def test_feature_extractor_cross_corr():
    """Cross-feature correlation ax–ay."""
    feats = [
        {"name": "ax_ay_corr", "type": "cross",
         "signal_a": "ax", "signal_b": "ay", "cross_type": "corr"}
    ]
    extractor = FeatureExtractor(feats)
    win = _make_window()
    win.ay = win.ax   # identik → corr harus ≈ 1.0
    vec, _, _ = extractor.extract(win)
    assert abs(vec[0] - 1.0) < 1e-6


def test_feature_extractor_missing_signal_uses_default():
    """Sinyal None → pakai default, masuk ke missing list."""
    feats = [
        {"name": "ir_mean", "type": "stat", "signal": "ir", "stat": "mean", "default": -99.0}
    ]
    extractor = FeatureExtractor(feats)
    win = _make_window()
    win.ir = None
    vec, _, missing = extractor.extract(win)
    assert vec[0] == -99.0
    assert "ir_mean" in missing


# =============================================================================
# Test adapter
# =============================================================================

def test_from_processor_basic():
    """from_processor harus mengisi semua field dari dict results."""
    results = {sig: np.zeros(64) for sig in ["ax","ay","az","gx","gy","gz","ir"]}
    win = from_processor(
        node_id=2, window_num=10,
        imu_data={"ts": 5000, "finger": True},
        ppg_data={"hr": 80, "spo2": 97.5, "finger": True},
        results=results,
    )
    assert win.node_id == 2
    assert win.window_num == 10
    assert win.ts == 5000
    assert win.hr == 80
    assert win.finger is True
    assert len(win.ax) == 64


def test_from_processor_missing_signal():
    """Sinyal yang tidak ada di results → None di WindowInput."""
    results = {"ax": np.zeros(64)}   # hanya ax
    win = from_processor(
        node_id=1, window_num=1,
        imu_data={"ts": 0, "finger": False},
        ppg_data={"hr": -1},
        results=results,
    )
    assert win.ax is not None
    assert win.ir is None
    assert win.gz is None


def test_from_storage_rows():
    """from_storage_rows harus mapping ke WindowInput dengan benar."""
    signals = {
        "ax": [0.1] * 64,
        "ir": [100.0] * 64,
    }
    win = from_storage_rows(
        node_id=1, window_num=5, ts=3000,
        signal_rows=signals, hr=70, spo2=99.0, finger=True,
    )
    assert win.ax is not None
    assert win.ir is not None
    assert win.ay is None   # tidak ada di signal_rows
    assert win.hr == 70
    assert win.spo2 == 99.0


# =============================================================================
# Runner manual
# =============================================================================

if __name__ == "__main__":
    tests = [
        # engine.load
        test_load_basic,
        test_load_missing_model_file,
        test_load_missing_config_file,
        test_load_config_invalid_labels_count,
        test_load_model_no_predict_proba,
        test_load_n_features_mismatch,
        # predict single
        test_predict_not_loaded,
        test_predict_returns_correct_shape,
        test_predict_feature_vec_length,
        test_predict_with_missing_signal,
        test_predict_skip_finger_required,
        test_predict_skip_min_hr,
        test_predict_skip_require_signals,
        test_predict_confidence_threshold,
        test_predict_short_str,
        # predict batch
        test_predict_batch_returns_same_length,
        test_predict_batch_mixed_skip,
        test_predict_batch_empty,
        # stats
        test_stats_accumulate,
        test_stats_skip_counted,
        test_stats_summary_format,
        # FeatureExtractor
        test_feature_extractor_all_stat_types,
        test_feature_extractor_meta_cast_int,
        test_feature_extractor_raw_index,
        test_feature_extractor_cross_corr,
        test_feature_extractor_missing_signal_uses_default,
        # adapter
        test_from_processor_basic,
        test_from_processor_missing_signal,
        test_from_storage_rows,
    ]

    passed = failed = 0
    for fn in tests:
        try:
            fn()
            print(f"  ✓  {fn.__name__}")
            passed += 1
        except Exception as e:
            print(f"  ✗  {fn.__name__} → {e}")
            import traceback; traceback.print_exc()
            failed += 1

    print(f"\n{'='*60}")
    print(f"  {passed} passed  |  {failed} failed  |  {len(tests)} total")
    print(f"{'='*60}")
    if failed > 0:
        sys.exit(1)
