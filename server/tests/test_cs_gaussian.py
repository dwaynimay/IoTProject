"""Test konsistensi CS encoder: router, gaussian, lasso."""

import numpy as np
import pytest
import sys
import os

# Tambahkan server/ ke path agar import berjalan tanpa install package
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from core import reconstruct, PHI, THETA, PSI, CS_N, CS_M


def test_phi_shape():
    """Φ harus berukuran (M, N)."""
    assert PHI.shape == (CS_M, CS_N)


def test_theta_shape():
    """Θ = Φ·Ψ harus berukuran (M, N)."""
    assert THETA.shape == (CS_M, CS_N)


def test_psi_shape():
    """Ψ (basis DCT) harus berukuran (N, N)."""
    assert PSI.shape == (CS_N, CS_N)


def test_reconstruct_returns_correct_shape():
    """Rekonstruksi dari y(M,) harus menghasilkan x_hat(N,)."""
    y = np.random.randn(CS_M)
    x_hat = reconstruct(y)
    assert x_hat.shape == (CS_N,)


def test_reconstruct_zero_input():
    """Input nol harus menghasilkan output nol."""
    y = np.zeros(CS_M)
    x_hat = reconstruct(y)
    assert x_hat.shape == (CS_N,)
    assert np.allclose(x_hat, 0)


def test_reconstruct_list_input():
    """Harus menerima list Python, bukan hanya ndarray."""
    y = [0.0] * CS_M
    x_hat = reconstruct(y)
    assert x_hat.shape == (CS_N,)


def test_reconstruct_residual_reasonable():
    """Residual dari sinyal sparse (di domain DCT) harus kecil (< 0.5)."""
    rng = np.random.default_rng(42)
    s_true = np.zeros(CS_N)
    s_true[rng.choice(CS_N, 5, replace=False)] = rng.standard_normal(5)
    x_true = PSI @ s_true
    y = PHI @ x_true
    x_hat = reconstruct(y)
    residual = np.linalg.norm(y - PHI @ x_hat) / (np.linalg.norm(y) + 1e-9)
    assert residual < 0.5, f"Residual terlalu besar: {residual:.4f}"


def test_phi_deterministic():
    """Φ yang di-generate dua kali dengan seed sama harus identik."""
    from cs.gaussian import generate_phi
    from core import CS_PHI_SEED
    phi1 = generate_phi(CS_PHI_SEED, CS_M, CS_N)
    phi2 = generate_phi(CS_PHI_SEED, CS_M, CS_N)
    assert np.allclose(phi1, phi2)


def test_core_public_api_importable():
    """Semua simbol di __all__ harus bisa diimport dari server.core."""
    from core import (
        reconstruct, PHI, THETA, PSI, ALGORITHM_NAME,
        QualityAssessor, QualityFlag, SignalMetric, WindowReport,
        ValidatorRegistry, ValidationError,
        StorageManager,
        CS_N, CS_M, OMP_K, CS_PHI_SEED,
        IMU_SIGNALS, PPG_SIGNALS, SIGNALS,
        MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, TOPIC_BASE,
        DB_PATH, RETENTION_HOURS, LOG_LEVEL,
    )
    # Jika tidak ada ImportError, test ini pass
    assert ALGORITHM_NAME is not None
    assert isinstance(IMU_SIGNALS, list)
    assert isinstance(SIGNALS, list)
