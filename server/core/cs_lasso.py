# File: server/core/cs_lasso.py

# =============================================================================
# cs_lasso.py — Algoritma CS: Gaussian Random Φ + DCT Ψ + LASSO
# =============================================================================
#
# Ini adalah implementasi CS versi pertama (lama).
# Disimpan sebagai modul terpisah agar bisa dibandingkan dengan Gaussian/OMP.
#
# Pipeline:
#   Φ = Gaussian acak (LCG + Box-Muller) — identik dengan firmware versi lama
#   Ψ = DCT basis (scipy.fftpack.idct)
#   Θ = Φ · Ψ
#   ŝ = LASSO(y, Θ)     ← sparse coefficients
#   x̂ = Ψ · ŝ           ← rekonstruksi ke domain waktu
#
# Kapan pakai ini?
#   - Membandingkan performa vs metode OMP
#   - Firmware masih pakai CS_Sensor_gaussian_lasso.h (archive)
#   - Butuh rekonstruksi yang lebih smooth (LASSO cenderung over-smooth)
#
# ⚠️  SINKRONISASI FIRMWARE:
#   generate_phi() di sini IDENTIK dengan _generate() di
#   archive/firmware/CS_Sensor_gaussian_lasso.h — jangan ubah LCG constants.
# =============================================================================

import warnings
import numpy as np
from scipy.fftpack import idct

# Guard import sklearn — tidak wajib ada jika tidak pakai LASSO
try:
    from sklearn.linear_model import Lasso as _SklearnLasso
    _SKLEARN_AVAILABLE = True
except ImportError:
    _SKLEARN_AVAILABLE = False


# =============================================================================
# 1. Generate Φ — Gaussian via LCG + Box-Muller
# =============================================================================
def generate_phi(seed: int, m: int, n: int) -> np.ndarray:
    """
    Bangkitkan matrix pengukuran Φ (m × n) menggunakan LCG + Box-Muller.

    Identik byte-per-byte dengan _generate() di
    archive/firmware/CS_Sensor_gaussian_lasso.h.

    Args:
        seed : nilai awal LCG — HARUS sama dengan CS_PHI_SEED di firmware
        m    : jumlah pengukuran (baris Φ)
        n    : panjang sinyal (kolom Φ)

    Returns:
        phi : np.ndarray (m × n), dtype float64
    """
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)

        state = np.uint32(seed)

        def _lcg() -> float:
            nonlocal state
            # Overflow uint32 adalah by design — identik dengan C++ uint32_t wrap
            state = np.uint32(np.uint32(1664525) * state + np.uint32(1013904223))
            return float(np.uint32(state) >> np.uint32(1)) / 2_147_483_647.0

        def _gaussian() -> float:
            u1 = _lcg()
            u1 = max(u1, 1e-7)  # hindari log(0)
            u2 = _lcg()
            return float(np.sqrt(-2.0 * np.log(u1)) * np.cos(2.0 * np.pi * u2))

        phi = np.zeros((m, n), dtype=np.float64)
        for i in range(m):
            row  = np.array([_gaussian() for _ in range(n)], dtype=np.float64)
            norm = np.linalg.norm(row)
            if norm > 1e-10:
                row /= norm * np.sqrt(m)
            phi[i] = row

    return phi


# =============================================================================
# 2. Build Ψ dan Θ
# =============================================================================
def build_psi(n: int) -> np.ndarray:
    """Matrix IDCT orthonormal Ψ (n × n)."""
    return idct(np.eye(n), norm='ortho', axis=0)


def build_theta(phi: np.ndarray, n: int) -> tuple[np.ndarray, np.ndarray]:
    """
    Θ = Φ · Ψ

    Returns:
        theta : np.ndarray (m × n)
        psi   : np.ndarray (n × n)
    """
    psi   = build_psi(n)
    theta = phi @ psi
    return theta, psi


# =============================================================================
# 3. Rekonstruksi dengan LASSO
# =============================================================================
def reconstruct(
    y     : "list | np.ndarray",
    theta : np.ndarray,
    psi   : np.ndarray,
    alpha : float,
    max_iter : int = 5000,
    tol      : float = 1e-5,
) -> np.ndarray:
    """
    Rekonstruksi sinyal x̂ dari measurement y menggunakan LASSO.

    Args:
        y        : (m,) measurement dari sensor
        theta    : (m × n) sensing matrix dari build_theta()
        psi      : (n × n) DCT basis dari build_theta()
        alpha    : regularisasi LASSO (lebih besar = lebih sparse/smooth)
        max_iter : maksimum iterasi LASSO
        tol      : toleransi konvergensi LASSO

    Returns:
        x_hat : (n,) sinyal rekonstruksi

    Raises:
        ImportError: jika scikit-learn tidak terinstall
    """
    if not _SKLEARN_AVAILABLE:
        raise ImportError(
            "scikit-learn diperlukan untuk LASSO. "
            "Install dengan: pip install scikit-learn"
        )

    y_arr = np.array(y, dtype=np.float64)

    lasso = _SklearnLasso(
        alpha       = alpha,
        max_iter    = max_iter,
        fit_intercept = False,
        tol         = tol,
    )
    lasso.fit(theta, y_arr)

    return psi @ lasso.coef_