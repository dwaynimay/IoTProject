# =============================================================================
# server/core/cs_utils.py — Shared CS utilities
#
# Satu tempat untuk:
#   - generate_phi()         → bangkitkan matrix Φ (harus identik dengan ESP32)
#   - reconstruct()          → rekonstruksi sinyal dari measurement y
#   - reconstruct_default()  → pakai parameter default dari config.py
#   - PHI, Theta, Psi        → singleton, dibangkitkan sekali saat import
#
# Semua apps import dari sini — tidak ada duplikasi generate_phi lagi.
# =============================================================================

import warnings
import numpy as np
from scipy.fftpack import idct
from sklearn.linear_model import Lasso

from .config import CS_N, CS_M, CS_PHI_SEED, LASSO_ALPHA, LASSO_MAX_ITER, LASSO_TOL


def generate_phi(seed: int, m: int, n: int) -> np.ndarray:
    """
    Bangkitkan matrix pengukuran Φ (m × n) menggunakan LCG + Box-Muller.

    ⚠ KRITIS: Implementasi ini HARUS identik byte-per-byte dengan
    _generatePhi() di CS_Sensor.h (ESP32). Jika berbeda, rekonstruksi gagal
    total (korelasi mendekati 0).

    Overflow uint32 pada LCG adalah by design — suppress RuntimeWarning.
    """
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)

        state = np.uint32(seed)

        def lcg() -> float:
            nonlocal state
            state = np.uint32(np.uint32(1664525) * state + np.uint32(1013904223))
            return float(np.uint32(state) >> np.uint32(1)) / 2147483647.0

        def gaussian() -> float:
            u1 = lcg()
            if u1 < 1e-7:
                u1 = 1e-7
            u2 = lcg()
            return float(np.sqrt(-2.0 * np.log(u1)) * np.cos(2.0 * np.pi * u2))

        phi = np.zeros((m, n), dtype=np.float64)
        for i in range(m):
            row  = np.array([gaussian() for _ in range(n)], dtype=np.float64)
            norm = np.linalg.norm(row)
            if norm > 1e-10:
                row /= (norm * np.sqrt(m))
            phi[i] = row

    return phi


def reconstruct(y: list, theta: np.ndarray, psi: np.ndarray,
                alpha: float = LASSO_ALPHA) -> np.ndarray:
    """
    Rekonstruksi sinyal x̂ dari measurement y menggunakan LASSO.

    Args:
        y     : list atau array panjang CS_M (measurement dari sensor)
        theta : matrix Theta = Phi @ Psi (CS_M × CS_N)
        psi   : matrix DCT basis (CS_N × CS_N)
        alpha : regularisasi LASSO

    Returns:
        x_hat : sinyal rekonstruksi panjang CS_N
    """
    y_arr = np.array(y, dtype=np.float64)
    lasso = Lasso(alpha=alpha, max_iter=LASSO_MAX_ITER,
                  fit_intercept=False, tol=LASSO_TOL)
    lasso.fit(theta, y_arr)
    return psi @ lasso.coef_


# =============================================================================
# Singleton — dibangkitkan sekali saat module pertama kali di-import
# Semua apps pakai ini; tidak perlu generate ulang.
# =============================================================================
print(f"[cs_utils] Membangkitkan Φ (M={CS_M}, N={CS_N}, seed={CS_PHI_SEED})...",
      end=" ", flush=True)

PHI   = generate_phi(CS_PHI_SEED, CS_M, CS_N)          # (CS_M × CS_N)
PSI   = idct(np.eye(CS_N), norm='ortho', axis=0)        # (CS_N × CS_N)
THETA = PHI @ PSI                                        # (CS_M × CS_N)

print("OK")


def reconstruct_default(y: list) -> np.ndarray:
    """
    Shortcut: rekonstruksi dengan singleton THETA, PSI, dan LASSO_ALPHA default.
    Pakai ini di hampir semua kasus.
    """
    return reconstruct(y, THETA, PSI, LASSO_ALPHA)