"""
server/verify_phi.py
Verifikasi bahwa Φ di Python identik dengan Φ di firmware ESP32.

Jalankan:
    python -m server.verify_phi

Lalu bandingkan output dengan yang tercetak di Serial Monitor ESP32
saat boot (tambahkan Serial.printf di CS_Sensor.h _generate() jika perlu).
"""

import numpy as np
from core.cs_utils import generate_phi
from core.config   import CS_N, CS_M, CS_PHI_SEED

phi = generate_phi()

print(f"Φ shape  : {phi.shape}  (M×N = {CS_M}×{CS_N})")
print(f"Seed     : {CS_PHI_SEED}")
print(f"Norm baris[0..3]: {np.linalg.norm(phi[:4], axis=1).round(6)}")
print(f"  (harus semua = {1/np.sqrt(CS_M):.6f} = 1/√M)")
print()
print("Φ[0, 0:8] =", phi[0, :8].round(6))
print("Φ[1, 0:8] =", phi[1, :8].round(6))
print("Φ[2, 0:8] =", phi[2, :8].round(6))
print()
print("Tambahkan ke CS_Sensor.h _generate() setelah selesai build Φ:")
print("  Serial.printf(\"PHI[0][0..3]: %.6f %.6f %.6f %.6f\\n\",")
print("    _phi[0][0], _phi[0][1], _phi[0][2], _phi[0][3]);")
print()
print("Lalu bandingkan dengan baris 'Φ[0, 0:8]' di atas.")
print("Jika cocok → sinkronisasi OK. Jika beda → cek LCG / Fisher-Yates.")

# Sanity check: Φ harus memenuhi RIP — cek ortogonalitas approx
gram = phi @ phi.T   # (M × M)
off_diag = gram - np.diag(np.diag(gram))
print(f"\nSanity check RIP:")
print(f"  max |off-diag(Φ·Φᵀ)| = {np.max(np.abs(off_diag)):.6f}  (harusnya kecil)")
print(f"  diag min/max          = {np.diag(gram).min():.6f} / {np.diag(gram).max():.6f}")
print(f"  (diag = 1/M = {1/CS_M:.6f} jika normalisasi benar)")
