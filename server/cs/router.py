# File: server/cs/router.py

# =============================================================================
# router.py — Router Algoritma CS
# =============================================================================
#
# Modul ini adalah SATU-SATUNYA interface yang perlu dipakai oleh apps/.
# apps/ tidak boleh import langsung dari hadamard.py.
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
#   from cs.router import reconstruct, PHI, THETA, PSI
#   x_hat = reconstruct(y)   # langsung, tanpa tahu algoritmanya apa
#
# CARA GANTI ALGORITMA:
#   Edit server/core/config.py → ubah CS_ALGORITHM = "hadamard"
# =============================================================================

import logging

from core.config import (
    CS_N, CS_M, CS_PHI_SEED, OMP_K,
    CS_ALGORITHM,
)

_logger = logging.getLogger(__name__)


# =============================================================================
# Validasi konfigurasi algoritma
# =============================================================================
_SUPPORTED_ALGORITHMS = ("hadamard",)

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

from . import hadamard as _algo

PHI             = _algo.generate_phi(CS_PHI_SEED, CS_M, CS_N)
THETA, PSI      = _algo.build_theta(PHI, CS_N)
ALGORITHM_NAME  = "Hadamard + DCT + OMP"

def reconstruct(y: "list | np.ndarray") -> "np.ndarray":
    """
    Rekonstruksi sinyal menggunakan OMP.

    Args:
        y : (m,) measurement vector dari sensor

    Returns:
        x_hat : (n,) sinyal rekonstruksi
    """
    return _algo.reconstruct(y, THETA, PSI, OMP_K)

import numpy as np  # noqa: E402 — import setelah branch agar tidak unused
_logger.info("%s | PHI=%s | THETA=%s", ALGORITHM_NAME, PHI.shape, THETA.shape)
