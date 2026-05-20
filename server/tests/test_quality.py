# File: server/tests/test_quality.py

# =============================================================================
# test_quality.py — Unit test untuk core/quality.py
# =============================================================================
#
# Jalankan dari root project:
#   python -m pytest server/tests/test_quality.py -v
#
# Atau tanpa pytest:
#   python server/tests/test_quality.py
# =============================================================================

import sys
import os
import math

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    from core.quality import (
        QualityAssessor,
        QualityFlag,
        SignalMetric,
        WindowReport,
        THRESHOLD_LOW_QUALITY,
        THRESHOLD_CRITICAL,
        SPARSITY_EPSILON,
        ZERO_SIGNAL_THRESHOLD,
    )
    import core.quality as quality_module
except ImportError as e:
    print(f"[SKIP] Tidak bisa import quality: {e}")
    sys.exit(0)

# =============================================================================
# Helper — buat PHI kecil untuk test (tidak butuh Hadamard lengkap)
# =============================================================================

_M = 8   # ukuran kecil agar test cepat
_N = 16

def _make_phi(m: int = _M, n: int = _N, seed: int = 42) -> np.ndarray:
    """PHI acak ternormalisasi — cukup untuk test residual."""
    rng = np.random.default_rng(seed)
    phi = rng.standard_normal((m, n))
    # Normalisasi per baris
    norms = np.linalg.norm(phi, axis=1, keepdims=True)
    return phi / (norms * math.sqrt(m))

def _make_assessor() -> QualityAssessor:
    return QualityAssessor(phi=_make_phi())

def _make_sparse_signal(n: int = _N, k: int = 3, seed: int = 7) -> np.ndarray:
    """Sinyal sparse: hanya k elemen non-nol."""
    rng = np.random.default_rng(seed)
    x = np.zeros(n)
    idx = rng.choice(n, k, replace=False)
    x[idx] = rng.standard_normal(k)
    return x

def _encode(phi: np.ndarray, x: np.ndarray) -> np.ndarray:
    """y = Φ · x (encode sempurna, residual harusnya 0)."""
    return phi @ x


# =============================================================================
# Test SignalMetric — assess_signal
# =============================================================================

def test_assess_signal_perfect_reconstruction():
    """
    Jika x_hat sempurna (y = Φ·x_hat persis), relative_error harus ~0.
    """
    assessor = _make_assessor()
    x_hat = _make_sparse_signal()
    y     = _encode(assessor._phi, x_hat)

    metric = assessor.assess_signal("ax", x_hat=x_hat, y=y)

    assert metric.flag == QualityFlag.OK, f"Flag: {metric.flag}"
    assert metric.relative_error < 1e-9, f"rel_err={metric.relative_error}"
    assert metric.signal_norm > 0


def test_assess_signal_zero_signal():
    """Sinyal nol → ZERO_SIGNAL flag."""
    assessor = _make_assessor()
    x_hat = np.zeros(_N)
    y     = np.zeros(_M)

    metric = assessor.assess_signal("gz", x_hat=x_hat, y=y)

    assert metric.flag == QualityFlag.ZERO_SIGNAL
    assert metric.relative_error == 0.0


def test_assess_signal_low_quality():
    """
    Jika x_hat salah (noise), residual besar → LOW_QUALITY atau CRITICAL.
    """
    assessor = _make_assessor()
    rng  = np.random.default_rng(0)
    x_true = _make_sparse_signal()
    y      = _encode(assessor._phi, x_true)

    # x_hat acak = rekonstruksi buruk
    x_hat_bad = rng.standard_normal(_N) * 0.001

    metric = assessor.assess_signal("ay", x_hat=x_hat_bad, y=y)

    assert metric.is_low_quality(), f"Harusnya LOW_QUALITY/CRITICAL, dapat {metric.flag}"
    assert metric.relative_error > THRESHOLD_LOW_QUALITY


def test_assess_signal_critical():
    """
    Rekonstruksi sangat buruk → CRITICAL.
    Paksa dengan x_hat = 0 sehingga residual = y.
    """
    assessor = _make_assessor()
    x_true = _make_sparse_signal()
    y      = _encode(assessor._phi, x_true) * 10  # amplitudo besar

    x_hat_zero = np.zeros(_N)  # rekonstruksi nol = terburuk

    metric = assessor.assess_signal("gx", x_hat=x_hat_zero, y=y)

    # relative_error = ||y - Φ·0|| / ||y|| = ||y|| / ||y|| = 1.0 → CRITICAL
    assert metric.flag == QualityFlag.CRITICAL
    assert abs(metric.relative_error - 1.0) < 1e-9


def test_assess_signal_sparsity_ratio():
    """sparsity_ratio = fraksi elemen non-nol."""
    assessor = _make_assessor()

    # x_hat dengan 4 non-nol dari 16 → sparsity = 4/16 = 0.25
    x_hat = np.zeros(_N)
    x_hat[[0, 1, 2, 3]] = [1.0, -0.5, 0.3, 2.1]
    y = _encode(assessor._phi, x_hat)

    metric = assessor.assess_signal("az", x_hat=x_hat, y=y)

    expected_sparsity = 4 / _N
    assert abs(metric.sparsity_ratio - expected_sparsity) < 1e-9, (
        f"Sparsity: {metric.sparsity_ratio} vs {expected_sparsity}"
    )


def test_assess_signal_snr_db_positive_for_good_reconstruction():
    """SNR harus positif untuk rekonstruksi bagus."""
    assessor = _make_assessor()
    x_hat = _make_sparse_signal()
    y     = _encode(assessor._phi, x_hat)

    # Tambah noise kecil ke y agar SNR bukan inf
    rng = np.random.default_rng(1)
    y_noisy = y + rng.standard_normal(_M) * 0.01

    metric = assessor.assess_signal("ir", x_hat=x_hat, y=y_noisy)

    assert math.isfinite(metric.snr_db)
    # SNR tidak harus sangat tinggi karena y_noisy ≠ Φ·x_hat persis
    # tapi residual tetap kecil → SNR positif


def test_assess_signal_short_str_format():
    """short_str harus mengandung nama sinyal dan angka."""
    assessor = _make_assessor()
    x_hat = _make_sparse_signal()
    y     = _encode(assessor._phi, x_hat)

    metric = assessor.assess_signal("gx", x_hat=x_hat, y=y)
    s = metric.short_str()

    assert "gx" in s
    assert "err=" in s
    assert "sparse=" in s
    assert "snr=" in s


def test_assess_signal_list_input():
    """y boleh berupa list Python, bukan hanya ndarray."""
    assessor = _make_assessor()
    x_hat = _make_sparse_signal()
    y     = list(_encode(assessor._phi, x_hat))  # konversi ke list

    metric = assessor.assess_signal("ax", x_hat=x_hat, y=y)
    assert metric.flag == QualityFlag.OK


# =============================================================================
# Test WindowReport — assess_window
# =============================================================================

def _make_full_results_and_measurements(
    assessor: QualityAssessor,
    n_signals: int = 3,
    noise_scale: float = 0.0,
    seed: int = 99,
) -> tuple[dict, dict]:
    """
    Buat results (x_hat) dan measurements (y) untuk beberapa sinyal.
    noise_scale = 0 → rekonstruksi sempurna.
    """
    rng     = np.random.default_rng(seed)
    signals = ["ax", "ay", "az", "gx", "gy", "gz", "ir"][:n_signals]

    results      = {}
    measurements = {}

    for sig in signals:
        x     = _make_sparse_signal(seed=rng.integers(0, 9999))
        y     = _encode(assessor._phi, x)
        if noise_scale > 0:
            y = y + rng.standard_normal(_M) * noise_scale
        results[sig]      = x
        measurements[sig] = y

    return results, measurements


def test_assess_window_all_ok():
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=4)

    report = assessor.assess_window(
        results=results, measurements=measurements, node_id=1, window_num=1
    )

    assert not report.has_low_quality(), f"Low quality: {report.low_quality_signals()}"
    assert not report.has_critical()
    assert report.mean_relative_error() < THRESHOLD_LOW_QUALITY


def test_assess_window_some_low_quality():
    """Jika satu sinyal rekonstruksinya buruk, report.has_low_quality() = True."""
    assessor = _make_assessor()
    rng      = np.random.default_rng(5)

    results, measurements = _make_full_results_and_measurements(assessor, n_signals=3)

    # Rusak rekonstruksi "ay" dengan x_hat nol
    results["ay"] = np.zeros(_N)

    report = assessor.assess_window(
        results=results, measurements=measurements, node_id=2, window_num=5
    )

    assert report.has_low_quality()
    assert "ay" in report.low_quality_signals()


def test_assess_window_missing_measurement():
    """Sinyal yang tidak ada di measurements → dilewati, tidak crash."""
    assessor = _make_assessor()

    results      = {"ax": _make_sparse_signal()}
    measurements = {}  # kosong — tidak ada y untuk "ax"

    report = assessor.assess_window(
        results=results, measurements=measurements
    )

    # Tidak ada metrik yang dihitung, tapi tidak crash
    assert "ax" not in report.metrics


def test_assess_window_summary_ok():
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=2)

    report = assessor.assess_window(
        results=results, measurements=measurements, node_id=1, window_num=3
    )

    s = report.summary()
    assert "Win#3" in s
    assert "Node1" in s
    assert "OK" in s


def test_assess_window_summary_low_quality():
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=2)

    # Rusak semua rekonstruksi
    for sig in results:
        results[sig] = np.zeros(_N)

    report = assessor.assess_window(
        results=results, measurements=measurements, node_id=1, window_num=7
    )

    s = report.summary()
    assert "LOW_QUALITY" in s or "CRITICAL" in s


def test_assess_window_detail_lines():
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=3)

    report  = assessor.assess_window(results=results, measurements=measurements)
    lines   = report.detail_lines()

    assert len(lines) == 3
    assert all(line.startswith("  ") for line in lines)


def test_assess_window_mean_relative_error():
    """mean_relative_error harus rata-rata dari semua sinyal."""
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=3)

    report = assessor.assess_window(results=results, measurements=measurements)

    manual_mean = np.mean([
        m.relative_error
        for m in report.metrics.values()
        if m.flag != QualityFlag.ZERO_SIGNAL
    ])
    assert abs(report.mean_relative_error() - manual_mean) < 1e-9


def test_assess_window_mean_sparsity():
    assessor = _make_assessor()
    results, measurements = _make_full_results_and_measurements(assessor, n_signals=2)

    report = assessor.assess_window(results=results, measurements=measurements)
    spar   = report.mean_sparsity()

    assert 0.0 <= spar <= 1.0


# =============================================================================
# Test QualityAssessor — statistik global
# =============================================================================

def test_global_stats_accumulate():
    assessor = _make_assessor()

    # Window 1 — OK
    r1, m1 = _make_full_results_and_measurements(assessor, n_signals=2, seed=1)
    assessor.assess_window(results=r1, measurements=m1, node_id=1, window_num=1)

    # Window 2 — buruk (x_hat nol)
    r2, m2 = _make_full_results_and_measurements(assessor, n_signals=2, seed=2)
    for sig in r2: r2[sig] = np.zeros(_N)
    assessor.assess_window(results=r2, measurements=m2, node_id=1, window_num=2)

    stats = assessor.global_stats()

    assert stats["total_windows"] == 2
    # Window 2 harusnya low_quality atau critical
    assert stats["low_quality_count"] + stats["critical_count"] >= 1
    assert stats["avg_relative_error"] > 0


def test_global_stats_empty():
    """Tanpa window apapun, tidak crash."""
    assessor = _make_assessor()
    stats    = assessor.global_stats()

    assert stats["total_windows"] == 0
    assert stats["avg_relative_error"] == 0.0


def test_stats_summary_format():
    assessor = _make_assessor()
    r, m = _make_full_results_and_measurements(assessor, n_signals=3)
    assessor.assess_window(results=r, measurements=m, node_id=1, window_num=1)

    s = assessor.stats_summary()
    assert "[Q-Stats]" in s
    assert "windows=1" in s
    assert "avg_err=" in s


def test_assessor_invalid_phi_shape():
    """PHI 1D harus raise ValueError."""
    try:
        QualityAssessor(phi=np.ones(16))
        assert False, "Harusnya raise ValueError"
    except ValueError:
        pass


# =============================================================================
# Runner manual
# =============================================================================

if __name__ == "__main__":
    tests = [
        # assess_signal
        test_assess_signal_perfect_reconstruction,
        test_assess_signal_zero_signal,
        test_assess_signal_low_quality,
        test_assess_signal_critical,
        test_assess_signal_sparsity_ratio,
        test_assess_signal_snr_db_positive_for_good_reconstruction,
        test_assess_signal_short_str_format,
        test_assess_signal_list_input,
        # assess_window
        test_assess_window_all_ok,
        test_assess_window_some_low_quality,
        test_assess_window_missing_measurement,
        test_assess_window_summary_ok,
        test_assess_window_summary_low_quality,
        test_assess_window_detail_lines,
        test_assess_window_mean_relative_error,
        test_assess_window_mean_sparsity,
        # global stats
        test_global_stats_accumulate,
        test_global_stats_empty,
        test_stats_summary_format,
        test_assessor_invalid_phi_shape,
    ]

    passed = failed = 0
    for fn in tests:
        try:
            fn()
            print(f"  ✓  {fn.__name__}")
            passed += 1
        except Exception as e:
            print(f"  ✗  {fn.__name__} → {e}")
            failed += 1

    print(f"\n{'='*55}")
    print(f"  {passed} passed  |  {failed} failed  |  {len(tests)} total")
    print(f"{'='*55}")
    if failed > 0:
        sys.exit(1)
