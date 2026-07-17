import os
import sys
import numpy as np
import matplotlib.pyplot as plt

# Tambahkan direktori parent ke path
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from core import reconstruct, PHI, CS_N, CS_M, OMP_K

def main():
    # Set seed acak agar reproduktif
    np.random.seed(42)

    # 1. Bangkitkan Sinyal Sintetis PPG
    t = np.arange(CS_N) * (20.0 / 1000.0) # 20ms = 50Hz
    
    # PPG Sinyal: gabungan gelombang sistolik (1.2Hz) + diastolik/dicrotic notch (2.4Hz) + drift pernapasan (0.15Hz)
    ppg_systolic = np.sin(2.0 * np.pi * 1.2 * t)
    ppg_diastolic = 0.4 * np.sin(2.0 * np.pi * 2.4 * t + np.pi/4)
    ppg_drift = 0.2 * np.sin(2.0 * np.pi * 0.15 * t)
    ppg_noise = 0.05 * np.random.randn(CS_N)
    
    x_ppg = ppg_systolic + ppg_diastolic + ppg_drift + ppg_noise
    
    # 2. Bangkitkan Sinyal Sintetis IMU (Akselerometer selama berjalan/getaran tajam)
    # Langkah berjalan (2Hz) + getaran frekuensi tinggi (12Hz) + getaran kejutan (shock/impulse)
    imu_walk = np.sin(2.0 * np.pi * 2.0 * t)
    imu_tremor = 0.3 * np.sin(2.0 * np.pi * 12.0 * t)
    # Shock impuls tajam di tengah window (index 30-33)
    imu_shock = np.zeros(CS_N)
    imu_shock[30:34] = 2.0
    
    x_imu = imu_walk + imu_tremor + imu_shock
    
    # 3. Proses Kompresi (ESP32-style: centering + perkalian matriks)
    # --- PPG ---
    mean_ppg = np.mean(x_ppg)
    y_ppg = PHI @ (x_ppg - mean_ppg)
    
    # --- IMU ---
    mean_imu = np.mean(x_imu)
    y_imu = PHI @ (x_imu - mean_imu)
    
    # 4. Rekonstruksi (Server-style: OMP reconstruction + restore mean)
    # --- PPG ---
    x_hat_ppg_centered = reconstruct(y_ppg)
    x_hat_ppg = x_hat_ppg_centered + mean_ppg
    
    # --- IMU ---
    x_hat_imu_centered = reconstruct(y_imu)
    x_hat_imu = x_hat_imu_centered + mean_imu
    
    # 5. Hitung Metrik
    rmse_ppg = np.linalg.norm(x_ppg - x_hat_ppg) / (np.linalg.norm(x_ppg) + 1e-9)
    mse_ppg = np.mean((x_ppg - x_hat_ppg) ** 2)
    
    rmse_imu = np.linalg.norm(x_imu - x_hat_imu) / (np.linalg.norm(x_imu) + 1e-9)
    mse_imu = np.mean((x_imu - x_hat_imu) ** 2)
    
    print("=== METRIK REKONSTRUKSI CS HADAMARD ===")
    print(f"PPG  - RMSE: {rmse_ppg:.4f} | MSE: {mse_ppg:.6f}")
    print(f"IMU  - RMSE: {rmse_imu:.4f} | MSE: {mse_imu:.6f}")
    
    # 6. Plotting Sinyal
    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    
    # PPG Plot
    axes[0].plot(t * 1000.0, x_ppg, label='Original PPG (Raw)', color='#e91e63', linewidth=2)
    axes[0].plot(t * 1000.0, x_hat_ppg, label='Reconstructed PPG (CS)', color='#00bcd4', linestyle='--', linewidth=2)
    axes[0].set_title(f"Reconstruction Quality: PPG Signal (RMSE: {rmse_ppg:.4f}, MSE: {mse_ppg:.6f})", fontsize=12)
    axes[0].set_xlabel("Time (ms)")
    axes[0].set_ylabel("Amplitude")
    axes[0].grid(True, linestyle=':', alpha=0.6)
    axes[0].legend()
    
    # IMU Plot
    axes[1].plot(t * 1000.0, x_imu, label='Original IMU (Raw with Tremor & Shock)', color='#3f51b5', linewidth=2)
    axes[1].plot(t * 1000.0, x_hat_imu, label='Reconstructed IMU (CS)', color='#4caf50', linestyle='--', linewidth=2)
    axes[1].set_title(f"Reconstruction Quality: Dynamic IMU Signal (RMSE: {rmse_imu:.4f}, MSE: {mse_imu:.6f})", fontsize=12)
    axes[1].set_xlabel("Time (ms)")
    axes[1].set_ylabel("Amplitude")
    axes[1].grid(True, linestyle=':', alpha=0.6)
    axes[1].legend()
    
    plt.tight_layout()
    
    # Simpan ke folder tools dan folder artifacts
    output_path = os.path.join(os.path.dirname(__file__), 'ppg_imu_reconstruction.png')
    plt.savefig(output_path, dpi=150)
    print(f"Plot hasil visualisasi berhasil disimpan di: {output_path}")

    # Copy ke folder artifacts Gemini agar bisa dirender langsung
    artifacts_dir = r"C:\Users\TUFF GAMING\.gemini\antigravity-ide\brain\ef97666e-e0ce-4363-921b-c5641311ab20"
    if os.path.exists(artifacts_dir):
        dest_path = os.path.join(artifacts_dir, 'ppg_imu_reconstruction.png')
        import shutil
        shutil.copy(output_path, dest_path)
        print(f"Plot berhasil disalin ke folder artifacts: {dest_path}")

if __name__ == '__main__':
    main()
