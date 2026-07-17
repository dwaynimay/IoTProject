import numpy as np
from scipy.fftpack import idct

# =============================================================================
# COPIED DIRECTLY FROM HADAMARD.PY TO AVOID CIRCULAR IMPORT ISSUES
# =============================================================================
def _build_hadamard(n: int) -> np.ndarray:
    if n == 1:
        return np.array([[1.0]])
    h = np.array([[1.0]])
    while h.shape[0] < n:
        h = np.block([[h, h], [h, -h]])
    return h

def _lcg_generator(seed: int):
    state = seed & 0xFFFFFFFF
    while True:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state

def generate_phi(seed: int, m: int, n: int) -> np.ndarray:
    H   = _build_hadamard(n)
    gen = _lcg_generator(seed)
    d = np.array([(1 if (next(gen) & 1) else -1) for _ in range(n)], dtype=np.float64)
    idx = list(range(n))
    for i in range(m):
        r = next(gen)
        j = i + int(r % (n - i))
        idx[i], idx[j] = idx[j], idx[i]
    row_idx = sorted(idx[:m])
    scale = 1.0 / np.sqrt(float(n) * float(m))
    phi   = np.zeros((m, n), dtype=np.float64)
    for mi, row in enumerate(row_idx):
        phi[mi, :] = d[row] * scale * H[row, :]
    return phi

def build_psi(n: int) -> np.ndarray:
    return idct(np.eye(n), norm='ortho', axis=0)

def build_theta(phi: np.ndarray, n: int) -> tuple[np.ndarray, np.ndarray]:
    psi   = build_psi(n)
    theta = phi @ psi
    return theta, psi

def omp(y: np.ndarray, theta: np.ndarray, k: int) -> np.ndarray:
    m, n     = theta.shape
    residual = y.copy().astype(np.float64)
    support  = []
    s_hat    = np.zeros(n, dtype=np.float64)

    col_norms = np.linalg.norm(theta, axis=0)
    col_norms = np.where(col_norms < 1e-10, 1.0, col_norms)
    theta_n   = theta / col_norms

    for _ in range(k):
        corr          = np.abs(theta_n.T @ residual)
        corr[support] = -np.inf
        best          = int(np.argmax(corr))
        support.append(best)
        theta_s       = theta[:, support]
        coef, _, _, _ = np.linalg.lstsq(theta_s, y, rcond=None)
        residual = y - theta_s @ coef
        s_hat[:] = 0.0
        for i, idx in enumerate(support):
            s_hat[idx] = coef[i]
    return s_hat

def reconstruct(y, theta, psi, k) -> np.ndarray:
    y_arr  = np.array(y, dtype=np.float64)
    y_norm = np.linalg.norm(y_arr)
    if y_norm < 1e-9: return np.zeros(psi.shape[0])
    s_hat = omp(y_arr / y_norm, theta, k)
    return psi @ s_hat * y_norm

# =============================================================================
# RECONSTRUCTION SCRIPT
# =============================================================================
y = [0.1975, 0.0122, 0.0030, 0.0088, -0.0288, -0.0026, 0.0016, 0.0011, -0.2929, -0.0039, -0.0807, -0.0006, 0.0217, -0.0016, 0.0051, 0.0011, -0.0257, 0.1074, 0.0059, 0.0008, -0.0284, -0.0567, 0.0021, 0.0364, 0.0066, -0.1589, -0.0009, 0.0243, -0.0456, -0.0946, -0.0050, -0.0013]

M = 32
N = 64
SEED = 0
K = 10  # Tingkatkan sparsity level agar bisa merekonstruksi sinyal yang kurang sparse di DCT

phi = generate_phi(SEED, M, N)
theta, psi = build_theta(phi, N)
x_hat = reconstruct(y, theta, psi, K)

print("--- 10 NILAI PERTAMA HASIL REKONSTRUKSI (x_hat) ---")
for i in range(10):
    print(f"Index {i:02d}: {x_hat[i]:.4f}")

print("\n--- BANDINGKAN DENGAN SINUS ASLI (DI DALAM TEST SKETCH) ---")
# Data asli di test sketch: sinf(2.0f * PI_F * 1.0f * t);
t = np.arange(N) * (20.0 / 1000.0) # 20ms = 50Hz
x_asli = np.sin(2.0 * np.pi * 1.0 * t)

for i in range(10):
    print(f"Index {i:02d}: x_asli = {x_asli[i]:.4f} | x_hat = {x_hat[i]:.4f} | diff = {abs(x_hat[i] - x_asli[i]):.4f}")

error = np.mean((x_hat - x_asli)**2)
print(f"\nMean Squared Error (MSE): {error:.6f}")
