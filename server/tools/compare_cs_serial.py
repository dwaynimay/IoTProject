import os
import sys
import json
import argparse
import numpy as np
import matplotlib.pyplot as plt
import serial
from collections import deque

# Menambahkan direktori 'server' ke sys.path agar bisa import module 'core' dan 'cs'
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from core import reconstruct

# Konstanta
CS_N = 64
HISTORY_WINDOWS = 10  # Jumlah history window yang ditampilkan (10 * 64 = 640 sampel)
MAX_LEN = CS_N * HISTORY_WINDOWS

COLORS = {
    'ax': '#2196F3', # Blue
    'ay': '#4CAF50', # Green
    'az': '#FF9800', # Orange
    'gx': '#9C27B0', # Purple
    'gy': '#F44336', # Red
    'gz': '#00BCD4'  # Cyan
}

def main():
    parser = argparse.ArgumentParser(description="Live CS Accuracy Visualizer over Serial (Single Graph)")
    parser.add_argument("--port", type=str, required=True, help="Port COM ESP32 (contoh: COM3 atau /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baudrate (default 115200)")
    args = parser.parse_args()

    print(f"Menyambungkan ke {args.port} dengan baud {args.baud}...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print(f"Gagal membuka port {args.port}: {e}")
        return
    
    plt.ion() # Interactive mode
    fig, ax = plt.subplots(figsize=(15, 8))
    fig.suptitle(f"Perbandingan Live: Raw vs CS Reconstructed ({HISTORY_WINDOWS} Windows, Semua Sumbu)", fontsize=16)
    
    ax.set_xlabel("Indeks Sampel")
    ax.set_ylabel("Nilai Sensor (m/s² untuk Accel, °/s untuk Gyro)")
    ax.set_xlim(0, MAX_LEN - 1)
    ax.grid(True, linestyle=':', alpha=0.6)
    
    lines_raw = {}
    lines_cs = {}
    
    # Inisialisasi History
    history_raw = {axis: deque([0.0]*MAX_LEN, maxlen=MAX_LEN) for axis in COLORS}
    history_cs  = {axis: deque([0.0]*MAX_LEN, maxlen=MAX_LEN) for axis in COLORS}
    
    # Buat 12 garis (6 raw, 6 CS) dalam 1 grafik
    for axis, color in COLORS.items():
        line_r, = ax.plot([], [], linestyle='-', color=color, label=f'{axis.upper()} Raw', linewidth=1.5, alpha=0.8)
        line_c, = ax.plot([], [], linestyle='--', color=color, label=f'{axis.upper()} CS', linewidth=2.0)
        lines_raw[axis] = line_r
        lines_cs[axis] = line_c
        
    ax.legend(loc='upper right', fontsize='small', ncol=2)
    
    plt.tight_layout()
    print("Menunggu data dari ESP32... Pastikan ESP32 menjalankan firmware 'test_imu_raw_vs_cs'.")

    try:
        while True:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                
                # Abaikan log biasa dari ESP32
                if not line.startswith('{'):
                    print(f"[Log ESP32] {line}")
                    continue
                
                try:
                    data = json.loads(line)
                    if data.get("type") != "cs_test":
                        continue
                        
                    rmse_texts = []
                    
                    global_min = float('inf')
                    global_max = float('-inf')
                    
                    for axis in ['ax', 'ay', 'az', 'gx', 'gy', 'gz']:
                        raw_key  = f"{axis}_raw"
                        cs_key   = f"{axis}_cs"
                        mean_key = f"mean_{axis}"
                        
                        if raw_key not in data or cs_key not in data:
                            continue
                            
                        x_raw = np.array(data[raw_key])
                        y_cs  = np.array(data[cs_key])
                        mean_val = float(data.get(mean_key, 0.0))
                        
                        # 1. Jalankan Rekonstruksi OMP
                        x_hat_centered = reconstruct(y_cs.tolist())
                        
                        # 2. Kembalikan ke titik mean aslinya
                        x_hat = x_hat_centered + mean_val
                        
                        # 3. Update History Deque
                        history_raw[axis].extend(x_raw)
                        history_cs[axis].extend(x_hat)
                        
                        # 4. Update Data Plot
                        x_indices = np.arange(MAX_LEN)
                        lines_raw[axis].set_data(x_indices, list(history_raw[axis]))
                        lines_cs[axis].set_data(x_indices, list(history_cs[axis]))
                        
                        # Cari nilai min/max dari seluruh sumbu untuk autoscale Y-axis
                        current_raw = list(history_raw[axis])
                        current_cs  = list(history_cs[axis])
                        global_min = min(global_min, np.min(current_raw), np.min(current_cs))
                        global_max = max(global_max, np.max(current_raw), np.max(current_cs))
                        
                        # Hitung RMSE per sumbu
                        rmse = np.sqrt(np.mean((x_raw - x_hat)**2))
                        rmse_texts.append(f"{axis.upper()}: {rmse:.2f}")
                    
                    # Terapkan batas Y dinamis untuk keseluruhan grafik
                    if global_min != float('inf') and global_max != float('-inf'):
                        margin = (global_max - global_min) * 0.1
                        if margin == 0: margin = 1.0
                        ax.set_ylim(global_min - margin, global_max + margin)
                    
                    fig.canvas.draw()
                    fig.canvas.flush_events()
                    
                    print(f"Window diterima | RMSE -> " + " | ".join(rmse_texts))

                except json.JSONDecodeError:
                    print("[Parse Error] JSON tidak lengkap. Mungkin terpotong saat transmisi.")
                except Exception as e:
                    print(f"[Error Rekonstruksi] {e}")
                    
            plt.pause(0.01)
            
    except KeyboardInterrupt:
        print("\nKeluar dari visualizer...")
    finally:
        ser.close()
        plt.ioff()
        plt.show()

if __name__ == "__main__":
    main()
