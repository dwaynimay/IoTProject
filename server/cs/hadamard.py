# File: server/cs/hadamard.py

# =============================================================================
# hadamard.py — Algoritma CS: Hadamard Φ + DCT Ψ + OMP
# =============================================================================
#
# Ini adalah implementasi CS versi saat ini (aktif dipakai).
#
# Pipeline:
#   Φ = Hadamard (D · H, subsampled M baris)
#   Ψ = DCT basis (scipy.fftpack.idct) — murni real, cocok untuk sinyal IMU/PPG
#   Θ = Φ · Ψ
#   ŝ = OMP(y, Θ, K)    ← sparse coefficients via Orthogonal Matching Pursuit
#   x̂ = Ψ · ŝ           ← rekonstruksi ke domain waktu
#
# Keunggulan vs LASSO:
#   - Tidak butuh scikit-learn
#   - Lebih cepat untuk K kecil (OMP O(K·M·N) vs LASSO O(M·N·iter))
#   - Hasil lebih tajam (tidak over-smooth)
#
# ⚠️  SINKRONISASI FIRMWARE:
#   generate_phi() di sini IDENTIK dengan _generate() di
#   include/CS_Sensor.h (Hadamard version).
#   Verifikasi: python -m server.verify_phi
# =============================================================================

import numpy as np
from scipy.fftpack import idct


# =============================================================================
# 1. Hadamard Matrix — Deterministik via Sylvester Construction
# =============================================================================
def _build_hadamard(n: int) -> np.ndarray:
    """
    Bangkitkan Hadamard matrix orde n via Sylvester construction.

    H(1)  = [[1]]
    H(2k) = [[H(k),  H(k)],
              [H(k), -H(k)]]

    Args:
        n : orde matrix — HARUS pangkat 2

    Raises:
        ValueError: jika n bukan pangkat 2
    """
    if n == 1:
        return np.array([[1.0]])
    if n & (n - 1) != 0:
        raise ValueError(f"n harus pangkat 2, dapat {n}")

    h = np.array([[1.0]])
    while h.shape[0] < n:
        h = np.block([[h, h], [h, -h]])
    return h


# =============================================================================
# 2. LCG — Identik dengan Firmware CS_Sensor.h
# =============================================================================
def _lcg_generator(seed: int):
    """
    Linear Congruential Generator.

    Konstanta: multiplier=1664525, increment=1013904223 (Numerical Recipes).
    Identik dengan lcg() di CS_Sensor.h — JANGAN ubah konstanta ini.

    Yields:
        uint32 value per panggilan next()
    """
    state = seed & 0xFFFFFFFF
    while True:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state


# =============================================================================
# 3. Generate Φ — Hadamard
# =============================================================================
def generate_phi(seed: int, m: int, n: int) -> np.ndarray:
    """
    Bangkitkan Φ (m × n) Hadamard.

    Langkah konstruksi (identik dengan _generate() di CS_Sensor.h):
      1. H  = Hadamard(n)           — deterministik, elemen ±1
      2. d  = array ±1 dari bit LSB LCG(seed)
      3. A  = diag(d) · H           — setiap baris H diskalakan ±1
      4. idx = Fisher-Yates partial shuffle → pilih m baris
      5. row_idx = sorted(idx[:m])  — urutan deterministik
      6. phi[i] = d[row] / sqrt(n·m) * H[row, :]

    Args:
        seed : seed LCG — HARUS sama dengan CS_PHI_SEED di firmware
        m    : jumlah baris (pengukuran)
        n    : jumlah kolom (panjang sinyal) — harus pangkat 2

    Returns:
        phi : np.ndarray (m × n), dtype float64
    """
    H   = _build_hadamard(n)
    gen = _lcg_generator(seed)

    # Step 2: d[i] = +1 atau -1 dari bit LSB LCG
    d = np.array(
        [(1 if (next(gen) & 1) else -1) for _ in range(n)],
        dtype=np.float64
    )

    # Step 4: Fisher-Yates partial shuffle untuk pilih m baris dari n
    idx = list(range(n))
    for i in range(m):
        r = next(gen)
        j = i + int(r % (n - i))
        idx[i], idx[j] = idx[j], idx[i]

    # Step 5: sort agar deterministik (identik dengan Python sort di verify_phi)
    row_idx = sorted(idx[:m])

    # Step 6: bangun Φ dengan normalisasi 1/sqrt(n·m)
    scale = 1.0 / np.sqrt(float(n) * float(m))
    phi   = np.zeros((m, n), dtype=np.float64)
    for mi, row in enumerate(row_idx):
        phi[mi, :] = d[row] * scale * H[row, :]

    return phi


# =============================================================================
# 4. Build Ψ dan Θ
# =============================================================================
def build_psi(n: int) -> np.ndarray:
    """
    Matrix IDCT orthonormal Ψ (n × n).

    Kenapa DCT bukan DFT kompleks?
      Sinyal IMU/PPG bersifat real-valued. DFT menghasilkan koefisien kompleks
      yang perlu di-split real+imag — rawan bug skala.
      DCT menghasilkan representasi murni real, lebih stabil untuk OMP.
    """
    return idct(np.eye(n), norm='ortho', axis=0)


def build_theta(phi: np.ndarray, n: int) -> tuple[np.ndarray, np.ndarray]:
    """
    Θ = Φ · Ψ  (m × n), murni real.

    Returns:
        theta : np.ndarray (m × n) — sensing matrix untuk OMP
        psi   : np.ndarray (n × n) — untuk IDCT saat rebuild x̂
    """
    psi   = build_psi(n)
    theta = phi @ psi
    return theta, psi


# =============================================================================
# 5. OMP — Orthogonal Matching Pursuit
# =============================================================================
def omp(y: np.ndarray, theta: np.ndarray, k: int) -> np.ndarray:
    """
    Rekonstruksi koefisien sparse ŝ dari measurement y via OMP.

    Alur per iterasi:
      1. Normalisasi kolom Θ untuk pemilihan atom (korelasi lebih fair)
      2. Cari kolom paling berkorelasi dengan residual saat ini
      3. Tambah ke support set S
      4. Least squares: ŝ_S = argmin ||y - Θ_S · ŝ_S||₂
      5. Update residual: r = y - Θ_S · ŝ_S
      6. Ulangi k kali

    Catatan normalisasi:
      Normalisasi hanya dipakai saat memilih atom (step 2).
      Least squares di step 4 menggunakan Θ ASLI (tidak ternormalisasi)
      agar koefisien tidak perlu di-scale balik — menghindari double-scale.

    Args:
        y     : (m,) measurement
        theta : (m × n) sensing matrix
        k     : sparsity level (jumlah atom yang dipilih)

    Returns:
        s_hat : (n,) sparse coefficients di domain DCT
    """
    m, n     = theta.shape
    residual = y.copy().astype(np.float64)
    support  = []
    s_hat    = np.zeros(n, dtype=np.float64)

    # Pre-normalisasi kolom untuk pemilihan atom
    col_norms = np.linalg.norm(theta, axis=0)
    col_norms = np.where(col_norms < 1e-10, 1.0, col_norms)
    theta_n   = theta / col_norms  # hanya untuk step korelasi

    for _ in range(k):
        # Pilih atom paling berkorelasi dengan residual
        corr          = np.abs(theta_n.T @ residual)
        corr[support] = -np.inf  # atom yang sudah dipilih tidak dipilih lagi
        best          = int(np.argmax(corr))
        support.append(best)

        # Least squares pada submatrix Θ ASLI
        theta_s       = theta[:, support]
        coef, _, _, _ = np.linalg.lstsq(theta_s, y, rcond=None)

        # Update residual
        residual = y - theta_s @ coef

        # Simpan koefisien ke posisi yang sesuai
        s_hat[:] = 0.0
        for i, idx in enumerate(support):
            s_hat[idx] = coef[i]

    return s_hat


# =============================================================================
# 6. Rekonstruksi x̂ dari y
# =============================================================================
def reconstruct(
    y     : "list | np.ndarray",
    theta : np.ndarray,
    psi   : np.ndarray,
    k     : int,
) -> np.ndarray:
    """
    Rekonstruksi sinyal x̂ dari measurement y.

    Normalisasi y sebelum OMP:
      OMP bekerja optimal saat skala y dan kolom Θ sebanding.
      Normalisasi ke unit norm mencegah numerical issue saat sinyal
      sangat kecil (sensor diam) atau sangat besar (gerakan cepat).
      Skala asli dikembalikan setelah rekonstruksi.

    Args:
        y     : (m,) measurement dari sensor
        theta : (m × n) sensing matrix dari build_theta()
        psi   : (n × n) DCT basis dari build_theta()
        k     : sparsity level OMP

    Returns:
        x_hat : (n,) sinyal rekonstruksi (real-valued)
    """
    y_arr  = np.array(y, dtype=np.float64)
    y_norm = np.linalg.norm(y_arr)

    if y_norm < 1e-9:
        # Sinyal hampir nol — kembalikan nol tanpa jalankan OMP
        return np.zeros(psi.shape[0])

    s_hat = omp(y_arr / y_norm, theta, k)
    return psi @ s_hat * y_norm
