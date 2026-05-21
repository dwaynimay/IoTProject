# File: server/core/cs_router.py

# =============================================================================
# cs_router.py — Router Algoritma CS
# =============================================================================
#
# Modul ini adalah SATU-SATUNYA interface yang perlu dipakai oleh apps/.
# apps/ tidak boleh import langsung dari cs_lasso.py atau cs_gaussian.py.
#
# Keuntungan:
#   - Ganti algoritma cukup ubah CS_ALGORITHM di config.py
#   - apps/ tidak perlu tahu algoritma apa yang aktif
#   - Bisa switch algoritma tanpa restart server (via reload)
#
# Interface yang dijamin sama untuk semua algoritma:
#   reconstruct(y)  → np.ndarray (n,)
#   PHI             → np.ndarray (m × n)
#   THETA           → np.ndarray (m × n)
#   PSI             → np.ndarray (n × n)
#   ALGORITHM_NAME  → str
#
# CARA PAKAI di apps/:
#   from server.core.cs_router import reconstruct, PHI, THETA, PSI
#   x_hat = reconstruct(y)   # langsung, tanpa tahu algoritmanya apa
#
# CARA GANTI ALGORITMA:
#   Edit server/core/config.py → ubah CS_ALGORITHM = "lasso" atau "omp"
# =============================================================================

import logging

from .config import (
    CS_N, CS_M, CS_PHI_SEED, OMP_K,
    LASSO_ALPHA, LASSO_MAX_ITER, LASSO_TOL,
    CS_ALGORITHM,
)

_logger = logging.getLogger(__name__)


# =============================================================================
# Validasi konfigurasi algoritma
# =============================================================================
_SUPPORTED_ALGORITHMS = ("omp", "lasso")

if CS_ALGORITHM not in _SUPPORTED_ALGORITHMS:
    raise ValueError(
        f"CS_ALGORITHM='{CS_ALGORITHM}' tidak dikenal. "
        f"Pilihan valid: {_SUPPORTED_ALGORITHMS}"
    )


# =============================================================================
# Build singleton Φ, Θ, Ψ sesuai algoritma yang dikonfigurasi
# =============================================================================
_logger.info("Algoritma: %s | M=%d N=%d seed=%d",
             CS_ALGORITHM.upper(), CS_M, CS_N, CS_PHI_SEED)

if CS_ALGORITHM == "omp":
    from . import cs_gaussian as _algo

    PHI             = _algo.generate_phi(CS_PHI_SEED, CS_M, CS_N)
    THETA, PSI      = _algo.build_theta(PHI, CS_N)
    ALGORITHM_NAME  = "Hadamard-Gaussian + DCT + OMP"

    def reconstruct(y: "list | np.ndarray") -> "np.ndarray":
        """
        Rekonstruksi sinyal menggunakan OMP.

        Args:
            y : (m,) measurement vector dari sensor

        Returns:
            x_hat : (n,) sinyal rekonstruksi
        """
        return _algo.reconstruct(y, THETA, PSI, OMP_K)

elif CS_ALGORITHM == "lasso":
    from . import cs_lasso as _algo

    PHI             = _algo.generate_phi(CS_PHI_SEED, CS_M, CS_N)
    THETA, PSI      = _algo.build_theta(PHI, CS_N)
    ALGORITHM_NAME  = "Gaussian + DCT + LASSO"

    def reconstruct(y: "list | np.ndarray") -> "np.ndarray":
        """
        Rekonstruksi sinyal menggunakan LASSO.

        Args:
            y : (m,) measurement vector dari sensor

        Returns:
            x_hat : (n,) sinyal rekonstruksi
        """
        return _algo.reconstruct(
            y, THETA, PSI,
            alpha    = LASSO_ALPHA,
            max_iter = LASSO_MAX_ITER,
            tol      = LASSO_TOL,
        )

import numpy as np  # noqa: E402 — import setelah branch agar tidak unused
_logger.info("%s | PHI=%s | THETA=%s", ALGORITHM_NAME, PHI.shape, THETA.shape)