# File: server/tests/test_validator.py

# =============================================================================
# test_validator.py — Unit test untuk core/validator.py
# =============================================================================
#
# Jalankan dari root project:
#   python -m pytest server/tests/test_validator.py -v
#
# Atau tanpa pytest:
#   python server/tests/test_validator.py
# =============================================================================

import math
import sys
import os

# Tambah server/ ke path agar bisa import core.validator
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

# Stub config jika tidak ada (untuk CI / standalone)
try:
    from core.validator import (
        ValidatorRegistry,
        ValidationError,
        _layer1_schema,
        _layer2_length,
        _layer3_finite,
        _layer5_whitelist,
        _MonotonicityTracker,
        ALLOWED_NODE_IDS,
        CS_M,
        IMU_SIGNALS,
        PPG_SIGNALS,
        _IMU_REQUIRED_FIELDS,
        _PPG_REQUIRED_FIELDS,
    )
    import core.validator as validator_module
except ImportError as e:
    print(f"[SKIP] Tidak bisa import validator: {e}")
    sys.exit(0)


# =============================================================================
# Helper — buat payload valid
# =============================================================================

def _make_valid_imu(ts: int = 1000, node_id: int = 1) -> dict:
    """Buat payload cs_imu yang valid."""
    payload = {"ts": ts, "finger": True}
    for sig in IMU_SIGNALS:
        payload[sig] = [0.01 * i for i in range(CS_M)]
        payload[f"mean_{sig}"] = 0.0
    return payload


def _make_valid_ppg(ts: int = 1000, node_id: int = 1) -> dict:
    """Buat payload cs_ppg yang valid."""
    payload = {
        "ts": ts, "hr": 75, "spo2": 98.0,
        "ppg_valid": True, "finger": True, "mean_ir": 100.0,
    }
    for sig in PPG_SIGNALS:
        payload[sig] = [100.0 + i for i in range(CS_M)]
    return payload


# =============================================================================
# Test Layer 1 — Schema
# =============================================================================

def test_layer1_all_fields_present():
    payload = _make_valid_imu()
    errors = _layer1_schema(payload, _IMU_REQUIRED_FIELDS)
    assert errors == [], f"Harusnya tidak ada error, dapat: {errors}"

def test_layer1_missing_ts():
    payload = _make_valid_imu()
    del payload["ts"]
    errors = _layer1_schema(payload, _IMU_REQUIRED_FIELDS)
    assert any("ts" in e for e in errors), "Harus error field 'ts' hilang"

def test_layer1_missing_multiple():
    payload = {"ts": 1000}  # hampir kosong
    errors = _layer1_schema(payload, _IMU_REQUIRED_FIELDS)
    # Harus error karena banyak field hilang
    assert len(errors) > 0

def test_layer1_ppg_missing_hr():
    payload = _make_valid_ppg()
    del payload["hr"]
    errors = _layer1_schema(payload, _PPG_REQUIRED_FIELDS)
    assert any("hr" in e for e in errors)


# =============================================================================
# Test Layer 2 — Length
# =============================================================================

def test_layer2_correct_length():
    payload = _make_valid_imu()
    errors = _layer2_length(payload, list(IMU_SIGNALS))
    assert errors == []

def test_layer2_too_short():
    payload = _make_valid_imu()
    payload["ax"] = [0.0] * (CS_M - 1)  # kurang 1
    errors = _layer2_length(payload, list(IMU_SIGNALS))
    assert any("ax" in e and str(CS_M - 1) in e for e in errors)

def test_layer2_too_long():
    payload = _make_valid_imu()
    payload["gx"] = [0.0] * (CS_M + 5)
    errors = _layer2_length(payload, list(IMU_SIGNALS))
    assert any("gx" in e for e in errors)

def test_layer2_not_a_list():
    payload = _make_valid_imu()
    payload["ay"] = "bukan_list"
    errors = _layer2_length(payload, list(IMU_SIGNALS))
    assert any("ay" in e for e in errors)


# =============================================================================
# Test Layer 3 — Finite
# =============================================================================

def test_layer3_all_finite():
    payload = _make_valid_imu()
    errors = _layer3_finite(payload, list(IMU_SIGNALS))
    assert errors == []

def test_layer3_nan_in_signal():
    payload = _make_valid_imu()
    payload["az"][5] = float("nan")
    errors = _layer3_finite(payload, list(IMU_SIGNALS))
    assert any("az" in e and "NaN" in e for e in errors)

def test_layer3_inf_in_signal():
    payload = _make_valid_imu()
    payload["gz"][0] = float("inf")
    errors = _layer3_finite(payload, list(IMU_SIGNALS))
    assert any("gz" in e and "Inf" in e for e in errors)

def test_layer3_value_too_large():
    payload = _make_valid_imu()
    payload["gx"][10] = 2e7  # di atas MEASUREMENT_ABS_MAX
    errors = _layer3_finite(payload, list(IMU_SIGNALS))
    assert any("gx" in e and "batas" in e for e in errors)

def test_layer3_ppg_nan():
    payload = _make_valid_ppg()
    payload["ir"][0] = float("nan")
    errors = _layer3_finite(payload, list(PPG_SIGNALS))
    assert any("ir" in e for e in errors)


# =============================================================================
# Test Layer 4 — Monotonicity
# =============================================================================

def test_monotonicity_first_packet():
    tracker = _MonotonicityTracker()
    ok, msg = tracker.check(node_id=1, ts=1000)
    assert ok, "Paket pertama harus diterima"
    assert msg == ""

def test_monotonicity_increasing_ts():
    tracker = _MonotonicityTracker()
    tracker.check(1, 1000)
    ok, msg = tracker.check(1, 1640)  # maju 640ms (1 window)
    assert ok

def test_monotonicity_backward_ts():
    tracker = _MonotonicityTracker()
    tracker.check(1, 5000)
    ok, msg = tracker.check(1, 4500)  # mundur 500ms
    assert not ok
    assert "mundur" in msg

def test_monotonicity_small_backward_within_tolerance():
    tracker = _MonotonicityTracker()
    tracker.check(1, 5000)
    ok, msg = tracker.check(1, 4950)  # mundur 50ms, dalam toleransi 100ms
    assert ok, "Jitter kecil harus diterima"

def test_monotonicity_large_jump_forward():
    """Lompat besar ke depan = kemungkinan reboot, diterima dengan warning."""
    tracker = _MonotonicityTracker()
    tracker.check(1, 1000)
    ok, msg = tracker.check(1, 100_000)  # lompat 99 detik
    assert ok, "Lompat besar ke depan harus diterima"
    assert "WARN" in msg or "lompat" in msg.lower()

def test_monotonicity_different_nodes_independent():
    """Monotonicity tracking harus per-node, tidak saling pengaruh."""
    tracker = _MonotonicityTracker()
    tracker.check(1, 5000)
    tracker.check(2, 8000)
    # Node 1 maju
    ok1, _ = tracker.check(1, 5640)
    assert ok1
    # Node 2 mundur
    ok2, _ = tracker.check(2, 7000)
    assert not ok2


# =============================================================================
# Test Layer 5 — Whitelist
# =============================================================================

def test_whitelist_disabled():
    """Whitelist None = terima semua."""
    original = validator_module.ALLOWED_NODE_IDS
    try:
        validator_module.ALLOWED_NODE_IDS = None
        errors = _layer5_whitelist(99)
        assert errors == []
    finally:
        validator_module.ALLOWED_NODE_IDS = original

def test_whitelist_allowed():
    original = validator_module.ALLOWED_NODE_IDS
    try:
        validator_module.ALLOWED_NODE_IDS = {1, 2}
        errors = _layer5_whitelist(1)
        assert errors == []
    finally:
        validator_module.ALLOWED_NODE_IDS = original

def test_whitelist_denied():
    original = validator_module.ALLOWED_NODE_IDS
    try:
        validator_module.ALLOWED_NODE_IDS = {1, 2}
        errors = _layer5_whitelist(99)
        assert len(errors) == 1
        assert "99" in errors[0]
    finally:
        validator_module.ALLOWED_NODE_IDS = original


# =============================================================================
# Test ValidatorRegistry — integrasi semua layer
# =============================================================================

def test_registry_valid_imu():
    reg = ValidatorRegistry()
    ok, errors = reg.validate_imu(node_id=1, payload=_make_valid_imu(ts=1000))
    assert ok
    assert errors == []
    assert reg.stats["imu_ok"] == 1

def test_registry_valid_ppg():
    reg = ValidatorRegistry()
    ok, errors = reg.validate_ppg(node_id=1, payload=_make_valid_ppg(ts=1000))
    assert ok
    assert errors == []
    assert reg.stats["ppg_ok"] == 1

def test_registry_invalid_counts():
    reg = ValidatorRegistry()
    # Payload buruk — field kosong
    ok, errors = reg.validate_imu(node_id=1, payload={})
    assert not ok
    assert reg.stats["imu_invalid"] == 1
    assert reg.stats["imu_ok"] == 0

def test_registry_sequential_windows():
    """Simulasi beberapa window berturut-turut — ts maju terus."""
    reg = ValidatorRegistry()
    for i in range(10):
        ts = 1000 + i * 640  # 640ms per window (64 sampel × 10ms)
        ok, errors = reg.validate_imu(
            node_id=1,
            payload=_make_valid_imu(ts=ts)
        )
        assert ok, f"Window {i} harus valid, errors: {errors}"
    assert reg.stats["imu_ok"] == 10

def test_registry_stops_early_on_schema_error():
    """Jika schema gagal, layer length/finite tidak dijalankan."""
    reg = ValidatorRegistry()
    # Payload tidak ada ts — layer 1 langsung gagal
    payload = _make_valid_imu()
    del payload["ts"]
    ok, errors = reg.validate_imu(node_id=1, payload=payload)
    assert not ok
    # Error hanya dari schema, bukan dari layer lain
    assert all("schema" in e for e in errors), f"Unexpected errors: {errors}"

def test_registry_nan_caught():
    reg = ValidatorRegistry()
    payload = _make_valid_imu(ts=1000)
    payload["gx"][3] = float("nan")
    ok, errors = reg.validate_imu(node_id=1, payload=payload)
    assert not ok
    assert any("gx" in e for e in errors)

def test_registry_monotonicity_caught():
    reg = ValidatorRegistry()
    reg.validate_imu(node_id=1, payload=_make_valid_imu(ts=5000))
    # Kirim ts yang mundur
    ok, errors = reg.validate_imu(node_id=1, payload=_make_valid_imu(ts=4000))
    assert not ok
    assert any("monotonicity" in e for e in errors)


def test_invalid_payload_does_not_poison_monotonicity_tracker():
    reg = ValidatorRegistry()
    invalid = _make_valid_imu(ts=5000)
    invalid["gx"][0] = float("nan")
    ok, _ = reg.validate_imu(node_id=1, payload=invalid)
    assert not ok

    ok, errors = reg.validate_imu(node_id=1, payload=_make_valid_imu(ts=1000))
    assert ok, errors


def test_nonfinite_timestamp_rejected_without_crash():
    reg = ValidatorRegistry()
    payload = _make_valid_imu(ts=float("nan"))
    ok, errors = reg.validate_imu(node_id=1, payload=payload)
    assert not ok
    assert any("ts" in error for error in errors)


# =============================================================================
# Runner manual (tanpa pytest)
# =============================================================================

if __name__ == "__main__":
    tests = [
        # Layer 1
        test_layer1_all_fields_present,
        test_layer1_missing_ts,
        test_layer1_missing_multiple,
        test_layer1_ppg_missing_hr,
        # Layer 2
        test_layer2_correct_length,
        test_layer2_too_short,
        test_layer2_too_long,
        test_layer2_not_a_list,
        # Layer 3
        test_layer3_all_finite,
        test_layer3_nan_in_signal,
        test_layer3_inf_in_signal,
        test_layer3_value_too_large,
        test_layer3_ppg_nan,
        # Layer 4
        test_monotonicity_first_packet,
        test_monotonicity_increasing_ts,
        test_monotonicity_backward_ts,
        test_monotonicity_small_backward_within_tolerance,
        test_monotonicity_large_jump_forward,
        test_monotonicity_different_nodes_independent,
        # Layer 5
        test_whitelist_disabled,
        test_whitelist_allowed,
        test_whitelist_denied,
        # Registry
        test_registry_valid_imu,
        test_registry_valid_ppg,
        test_registry_invalid_counts,
        test_registry_sequential_windows,
        test_registry_stops_early_on_schema_error,
        test_registry_nan_caught,
        test_registry_monotonicity_caught,
    ]

    passed = 0
    failed = 0
    for test_fn in tests:
        try:
            test_fn()
            print(f"  ✓  {test_fn.__name__}")
            passed += 1
        except Exception as e:
            print(f"  ✗  {test_fn.__name__} → {e}")
            failed += 1

    print(f"\n{'='*50}")
    print(f"  {passed} passed  |  {failed} failed  |  {len(tests)} total")
    print(f"{'='*50}")
    if failed > 0:
        sys.exit(1)
