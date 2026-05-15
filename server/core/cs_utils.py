# =============================================================================
# server/core/cs_utils.py — CS utilities (Hadamard-Gaussian + DCT + OMP)
#
# Pipeline:
#   Φ  = Hadamard-Gaussian (D · H, subsampled M baris)
#   Ψ  = DCT basis (domain frekuensi, murni real — varian Fourier untuk sinyal real)
#   Θ  = Φ · Ψ  (sensing matrix untuk OMP, ukuran M × N, murni real)
#   ŝ  = OMP(y, Θ, K)   ← koefisien sparse di domain DCT
#   x̂  = Ψ · ŝ          ← rekonstruksi ke domain waktu
#
# Catatan basis:
#   DCT (Discrete Cosine Transform) adalah turunan DFT untuk sinyal real.
#   Untuk sinyal IMU/PPG yang real-valued, DCT lebih tepat daripada DFT kompleks
#   karena tidak ada bagian imajiner — tidak perlu split real/imag yang rawan bug.
#
# ⚠ SINKRONISASI FIRMWARE:
#   generate_phi() HARUS identik dengan _generate() di CS_Sensor.h.
#   Verifikasi: python -m server.verify_phi
# =============================================================================

import numpy as np
from scipy.fftpack import dct, idct
from .config import CS_N, CS_M, CS_PHI_SEED, OMP_K


# =============================================================================
# 1. Hadamard matrix (deterministik)
# =============================================================================
def _hadamard(n: int) -> np.ndarray:
    """Hadamard orde n via Sylvester. n harus pangkat 2."""
    if n == 1:
        return np.array([[1.0]])
    if n & (n - 1) != 0:
        raise ValueError(f"n harus pangkat 2, dapat {n}")
    h = np.array([[1.0]])
    while h.shape[0] < n:
        h = np.block([[h, h], [h, -h]])
    return h


# =============================================================================
# 2. LCG — identik dengan firmware CS_Sensor.h
# =============================================================================
def _lcg_rng(seed: int):
    """LCG: multiplier=1664525, increment=1013904223. Yield uint32."""
    state = seed & 0xFFFFFFFF
    while True:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state


# =============================================================================
# 3. Generate Φ — Hadamard-Gaussian
#    IDENTIK dengan _generate() di CS_Sensor.h
# =============================================================================
def generate_phi(seed: int = CS_PHI_SEED,
                 m: int   = CS_M,
                 n: int   = CS_N) -> np.ndarray:
    """
    Bangkitkan Φ (m × n) Hadamard-Gaussian.
    Identik byte-per-byte dengan firmware ESP32.
    """
    H   = _hadamard(n)
    gen = _lcg_rng(seed)

    # d[i] = ±1 dari bit LSB LCG
    d = np.array([(1 if (next(gen) & 1) else -1) for _ in range(n)],
                 dtype=np.float64)

    # Fisher-Yates partial shuffle
    idx = list(range(n))
    for i in range(m):
        r = next(gen)
        j = i + int(r % (n - i))
        idx[i], idx[j] = idx[j], idx[i]

    row_idx = sorted(idx[:m])

    # phi[i] = d[row] / sqrt(N*M) * H[row, :]
    scale = 1.0 / np.sqrt(float(n) * float(m))
    phi   = np.zeros((m, n), dtype=np.float64)
    for mi, row in enumerate(row_idx):
        phi[mi, :] = d[row] * scale * H[row, :]

    return phi


# =============================================================================
# 4. Basis sparse Ψ — DCT (varian Fourier untuk sinyal real)
#
# Ψ = matrix IDCT orthonormal (n × n), murni real.
# x = Ψ · s̃  ↔  s̃ = DCT(x)  (sparse di domain frekuensi)
#
# Kenapa DCT bukan DFT kompleks?
#   Sinyal IMU/PPG bersifat real-valued. DFT menghasilkan koefisien kompleks
#   yang harus di-split jadi real+imag — rawan bug skala.
#   DCT langsung menghasilkan representasi real, lebih stabil untuk OMP.
# =============================================================================
def build_psi(n: int = CS_N) -> np.ndarray:
    """
    Bangkitkan matrix IDCT orthonormal Ψ (n × n).
    Kolom ke-k = basis cosine frekuensi ke-k.
    """
    return idct(np.eye(n), norm='ortho', axis=0)  # (n × n), real


def build_theta(phi: np.ndarray, n: int = CS_N):
    """
    Θ = Φ · Ψ  (m × n), murni real.
    Dipakai langsung oleh OMP tanpa split kompleks.

    Returns:
        theta : np.ndarray (m × n)
        psi   : np.ndarray (n × n)  — untuk IDCT saat rebuild x̂
    """
    psi   = build_psi(n)
    theta = phi @ psi          # (m × n)
    return theta, psi


# =============================================================================
# 5. OMP — Orthogonal Matching Pursuit (implementasi bersih, tanpa double-scale)
# =============================================================================
def omp(y: np.ndarray,
        theta: np.ndarray,
        k: int = OMP_K) -> np.ndarray:
    """
    OMP rekonstruksi koefisien sparse ŝ dari measurement y.

    Alur per iterasi:
      1. Cari atom (kolom Θ) paling berkorelasi dengan residual
      2. Tambah ke support set S
      3. Least squares: ŝ_S = argmin ||y - Θ_S · ŝ_S||₂
      4. Update residual: r = y - Θ_S · ŝ_S
      5. Ulangi K kali

    Args:
        y     : (m,) measurement
        theta : (m × n) sensing matrix — TIDAK dinormalisasi di sini
        k     : sparsity level

    Returns:
        s_hat : (n,) sparse coefficients
    """
    m, n     = theta.shape
    residual = y.copy().astype(np.float64)
    support  = []
    s_hat    = np.zeros(n, dtype=np.float64)

    # Pre-normalisasi kolom hanya untuk pemilihan atom (korelasi)
    # Koefisien dihitung dari theta ASLI (bukan theta_n) — tidak ada double-scale
    col_norms = np.linalg.norm(theta, axis=0)
    col_norms = np.where(col_norms < 1e-10, 1.0, col_norms)

    for _ in range(k):
        # 1. Korelasi dengan kolom ternormalisasi
        corr = np.abs((theta / col_norms).T @ residual)   # (n,)
        corr[support] = -np.inf
        best = int(np.argmax(corr))
        support.append(best)

        # 2. Least squares pada submatrix theta ASLI (bukan ternormalisasi)
        theta_s              = theta[:, support]           # (m × |S|)
        coef, _, _, _        = np.linalg.lstsq(theta_s, y, rcond=None)

        # 3. Update residual
        residual = y - theta_s @ coef

        # 4. Simpan koefisien langsung — tidak perlu scale balik
        s_hat[:] = 0.0
        for i, idx in enumerate(support):
            s_hat[idx] = coef[i]

    return s_hat   # (n,)


# =============================================================================
# 6. Rekonstruksi x̂ dari y
# =============================================================================
def reconstruct(y:     "list | np.ndarray",
                theta: np.ndarray,
                psi:   np.ndarray,
                k:     int = OMP_K) -> np.ndarray:
    """
    Rekonstruksi sinyal x̂ dari measurement y.

    Normalisasi y sebelum OMP:
      OMP bekerja optimal ketika skala y dan kolom Θ sebanding.
      Jika amplitudo y sangat kecil (sensor diam) atau sangat besar
      (gerakan cepat), normalisasi mencegah numerical issue dan
      memastikan residual konsisten antar window.

    Args:
        y     : (m,) measurement dari sensor
        theta : (m × n) sensing matrix dari build_theta()
        psi   : (n × n) DCT basis dari build_theta()
        k     : sparsity level OMP

    Returns:
        x_hat : (n,) sinyal rekonstruksi (real)
    """
    y_arr = np.array(y, dtype=np.float64)

    # Normalisasi y — scale ke unit norm, kembalikan skala setelah rekonstruksi
    y_norm = np.linalg.norm(y_arr)
    if y_norm > 1e-9:
        y_scaled = y_arr / y_norm
        s_hat    = omp(y_scaled, theta, k)
        x_hat    = psi @ s_hat * y_norm   # kembalikan skala asli
    else:
        # Sinyal hampir nol — kembalikan nol
        x_hat = np.zeros(psi.shape[0])

    return x_hat


# =============================================================================
# Singleton — di-build sekali saat module di-import
# =============================================================================
print(f"[cs_utils] Hadamard-Gaussian Φ + DCT Ψ + OMP | "
      f"M={CS_M} N={CS_N} K={OMP_K} seed={CS_PHI_SEED}...",
      end=" ", flush=True)

PHI              = generate_phi()
THETA_REAL, PSI_C = build_theta(PHI)   # nama tetap untuk kompatibilitas apps

print("OK")


def reconstruct_default(y: "list | np.ndarray") -> np.ndarray:
    """Shortcut rekonstruksi dengan parameter default."""
    return reconstruct(y, THETA_REAL, PSI_C, OMP_K)