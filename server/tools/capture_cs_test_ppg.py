"""
capture_cs_test_ppg.py - Rekam hasil uji PPG raw vs compressive sensing dari serial ESP32.

Firmware pasangan:
    pio run -e test_ppg_raw_vs_cs -t upload

Contoh:
    .\server\.venv\Scripts\python.exe -m server.tools.capture_cs_test_ppg --port COM5 --windows 10
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import serial
from server.tools.ppg_dsp import HeartRateMonitor


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
SERVER_ROOT = PROJECT_ROOT / "server"
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))

from server.cs.hadamard import build_theta, generate_phi, reconstruct  # noqa: E402
from server.core.config import CS_PHI_SEED, OMP_K  # noqa: E402


CS_N = 64
CS_M = 32
PHI = generate_phi(CS_PHI_SEED, CS_M, CS_N)
THETA, PSI = build_theta(PHI, CS_N)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Tangkap data uji PPG raw vs CS dari serial ESP32."
    )
    parser.add_argument("--port", required=True, help="Port serial ESP32, contoh COM5")
    parser.add_argument("--baud", type=int, default=115200, help="Baudrate serial")
    parser.add_argument("--windows", type=int, default=10, help="Jumlah window valid yang direkam")
    parser.add_argument("--timeout", type=float, default=120.0, help="Batas waktu rekam dalam detik")
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Folder output. Default: server/data/cs_ppg_capture_<timestamp>",
    )
    return parser.parse_args()


def ensure_output_dir(out_dir: Path | None) -> Path:
    if out_dir is None:
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        out_dir = PROJECT_ROOT / "server" / "data" / f"cs_ppg_capture_{timestamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir


def calc_metrics(x_raw: np.ndarray, x_hat: np.ndarray) -> tuple[float, float, float, float]:
    error = x_raw - x_hat
    rmse = float(np.sqrt(np.mean(np.square(error))))
    mae = float(np.mean(np.abs(error)))
    signal_power = float(np.mean(np.square(x_raw)))
    noise_power = float(np.mean(np.square(error)))
    snr_db = 99.0 if noise_power <= 1e-12 else float(10.0 * math.log10(signal_power / noise_power))

    if np.std(x_raw) <= 1e-12 or np.std(x_hat) <= 1e-12:
        corrcoef = 1.0 if np.allclose(x_raw, x_hat) else 0.0
    else:
        corrcoef = float(np.corrcoef(x_raw, x_hat)[0, 1])
    return rmse, mae, snr_db, corrcoef


def capture(args: argparse.Namespace) -> list[dict]:
    windows: list[dict] = []
    hr_monitor_cs = HeartRateMonitor()
    now_ms = 0
    start_time = time.time()

    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        print(f"[INFO] Tersambung ke {args.port} @ {args.baud}")
        print(f"[INFO] Menunggu {args.windows} window valid dari firmware test_ppg_raw_vs_cs...")

        while len(windows) < args.windows:
            if time.time() - start_time > args.timeout:
                raise TimeoutError(
                    f"Waktu habis. Hanya menerima {len(windows)} dari {args.windows} window."
                )

            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            if not line.startswith("{"):
                print(f"[ESP32] {line}")
                continue

            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                print("[WARN] JSON serial terpotong, baris dilewati.")
                continue

            if payload.get("type") != "cs_test_ppg":
                continue

            ir_raw = np.array(payload["ir_raw"], dtype=float)
            ir_cs = np.array(payload["ir_cs"], dtype=float)
            mean_ir = float(payload.get("mean_ir", 0.0))

            x_hat_centered = reconstruct(ir_cs.tolist(), THETA, PSI, OMP_K)
            ir_reconstructed = np.array(x_hat_centered, dtype=float) + mean_ir

            rmse, mae, snr_db, corrcoef = calc_metrics(ir_raw, ir_reconstructed)

            # Continuous DC Stitching (Sambung Patah)
            # Agar batas window CS tidak membuat lonjakan yang memicu MotionGate,
            # kita geser (offset) seluruh nilai window ini agar persis nyambung
            # dengan ujung window sebelumnya.
            if len(windows) > 0:
                last_val = windows[-1]["ir_reconstructed"][-1]
                offset = last_val - ir_reconstructed[0]
                ir_reconstructed += offset

            if len(windows) == 0:
                hr_monitor_cs.onContact(ir_reconstructed[0], now_ms)
            
            for i in range(len(ir_reconstructed)):
                hr_monitor_cs.update(ir_reconstructed[i], now_ms)
                now_ms += 20
            
            hr_python_cs = float(hr_monitor_cs.getBpm())

            window_idx = len(windows) + 1
            windows.append(
                {
                    "window_idx": window_idx,
                    "ir_raw": ir_raw.tolist(),
                    "ir_cs": ir_cs.tolist(),
                    "ir_reconstructed": ir_reconstructed.tolist(),
                    "mean_ir": mean_ir,
                    "hr": payload.get("hr", -1),
                    "hr_python": hr_python_cs,
                    "spo2": payload.get("spo2", 0.0),
                    "ppg_valid": bool(payload.get("ppg_valid", False)),
                    "rmse": rmse,
                    "mae": mae,
                    "snr_db": snr_db,
                    "corrcoef": corrcoef,
                }
            )

            print(
                f"[OK] Window {window_idx}/{args.windows} | "
                f"RMSE={rmse:.4f} | MAE={mae:.4f} | SNR={snr_db:.2f} dB | "
                f"Corr={corrcoef:.4f} | HR={payload.get('hr', -1)} | SpO2={payload.get('spo2', 0.0)}"
            )

    return windows


def write_summary_csv(out_dir: Path, windows: list[dict]) -> Path:
    path = out_dir / "summary_metrics_ppg.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["window_idx", "mean_ir", "hr", "spo2", "ppg_valid", "rmse", "mae", "snr_db", "corrcoef"]
        )
        for window in windows:
            writer.writerow(
                [
                    window["window_idx"],
                    f"{window['mean_ir']:.6f}",
                    window["hr"],
                    f"{float(window['spo2']):.6f}",
                    int(window["ppg_valid"]),
                    f"{window['rmse']:.6f}",
                    f"{window['mae']:.6f}",
                    f"{window['snr_db']:.6f}",
                    f"{window['corrcoef']:.6f}",
                ]
            )
    return path


def write_sample_csv(out_dir: Path, windows: list[dict]) -> Path:
    path = out_dir / "sample_comparison_ppg.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["window_idx", "sample_idx", "raw_ir", "reconstructed_ir", "abs_error"])
        for window in windows:
            for sample_idx, (raw_value, rec_value) in enumerate(
                zip(window["ir_raw"], window["ir_reconstructed"])
            ):
                writer.writerow(
                    [
                        window["window_idx"],
                        sample_idx,
                        f"{raw_value:.6f}",
                        f"{rec_value:.6f}",
                        f"{abs(raw_value - rec_value):.6f}",
                    ]
                )
    return path


def write_measurement_csv(out_dir: Path, windows: list[dict]) -> Path:
    path = out_dir / "compressed_measurements_ppg.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["window_idx", "measurement_idx", "measurement_value"])
        for window in windows:
            for measurement_idx, value in enumerate(window["ir_cs"]):
                writer.writerow([window["window_idx"], measurement_idx, f"{value:.6f}"])
    return path


def plot_overlay(out_dir: Path, windows: list[dict]) -> Path:
    fig, ax = plt.subplots(figsize=(14, 6))
    for window in windows:
        x_axis = np.arange(CS_N)
        ax.plot(x_axis, window["ir_raw"], alpha=0.35, linewidth=1.0, color="#e91e63")
        ax.plot(x_axis, window["ir_reconstructed"], alpha=0.35, linewidth=1.0, linestyle="--", color="#00acc1")

    ax.set_title("Perbandingan PPG IR Raw vs Hasil Rekonstruksi CS")
    ax.set_xlabel("Indeks Sampel")
    ax.set_ylabel("Nilai IR")
    ax.grid(True, linestyle=":", alpha=0.4)
    ax.legend(
        handles=[
            plt.Line2D([0], [0], color="#e91e63", linewidth=2, label="IR Raw"),
            plt.Line2D([0], [0], color="#00acc1", linewidth=2, linestyle="--", label="IR Rekonstruksi"),
        ]
    )

    path = out_dir / "plot_overlay_ppg.png"
    fig.tight_layout()
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    return path

def plot_hr_spo2(out_dir: Path, windows: list[dict]) -> Path:
    fig, (ax_hr, ax_spo2) = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
    
    x_vals = [window["window_idx"] for window in windows]
    hr_vals = [window["hr"] for window in windows]
    hr_python_cs = [window.get("hr_python", 0.0) for window in windows]
    spo2_vals = [window["spo2"] for window in windows]
    
    ax_hr.plot(x_vals, hr_vals, marker="o", linewidth=3.0, color="#d32f2f", label="HR Asli (Firmware ESP32)")
    ax_hr.plot(x_vals, hr_python_cs, marker="x", linewidth=2.0, linestyle="--", color="#388e3c", label="HR Python (CS Reconstructed)")
    ax_hr.set_title("Perbandingan Detak Jantung (HR): Asli vs Rekonstruksi")
    ax_hr.set_ylabel("Heart Rate (BPM)")
    ax_hr.grid(True, linestyle=":", alpha=0.4)
    ax_hr.legend()
    
    ax_spo2.plot(x_vals, spo2_vals, marker="o", linewidth=2.0, color="#1976d2")
    ax_spo2.set_title("Tren Kadar Oksigen (SpO2) per Window")
    ax_spo2.set_xlabel("Window Index")
    ax_spo2.set_ylabel("SpO2 (%)")
    ax_spo2.grid(True, linestyle=":", alpha=0.4)
    
    path = out_dir / "plot_hr_spo2.png"
    fig.tight_layout()
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_metrics(out_dir: Path, windows: list[dict]) -> Path:
    fig, axes = plt.subplots(2, 2, figsize=(14, 8), sharex=True)
    metric_specs = [
        ("rmse", "RMSE"),
        ("mae", "MAE"),
        ("snr_db", "SNR (dB)"),
        ("corrcoef", "Korelasi"),
    ]

    x_vals = [window["window_idx"] for window in windows]
    for idx, (key, title) in enumerate(metric_specs):
        ax = axes[idx // 2][idx % 2]
        y_vals = [window[key] for window in windows]
        ax.plot(x_vals, y_vals, marker="o", linewidth=1.5, color="#3949ab")
        ax.set_title(title)
        ax.set_xlabel("Window")
        ax.grid(True, linestyle=":", alpha=0.4)

    fig.suptitle("Tren Metrik Rekonstruksi PPG per Window", fontsize=15)
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    path = out_dir / "plot_metric_trends_ppg.png"
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    return path


def write_readme(out_dir: Path, args: argparse.Namespace, windows: list[dict]) -> Path:
    path = out_dir / "README_HASIL_PPG.txt"
    mean_rmse = float(np.mean([window["rmse"] for window in windows]))
    mean_mae = float(np.mean([window["mae"] for window in windows]))
    mean_snr = float(np.mean([window["snr_db"] for window in windows]))
    mean_corr = float(np.mean([window["corrcoef"] for window in windows]))

    lines = [
        "HASIL UJI COMPRESSIVE SENSING PPG",
        "=================================",
        f"Port serial     : {args.port}",
        f"Baudrate        : {args.baud}",
        f"Jumlah window   : {len(windows)}",
        f"Panjang window  : {CS_N} sampel",
        f"Jumlah y CS     : {CS_M} pengukuran",
        "",
        "Ringkasan rata-rata:",
        f"- RMSE rata-rata      : {mean_rmse:.4f}",
        f"- MAE rata-rata       : {mean_mae:.4f}",
        f"- SNR rata-rata       : {mean_snr:.2f} dB",
        f"- Korelasi rata-rata  : {mean_corr:.4f}",
        "",
        "File output utama:",
        "- summary_metrics_ppg.csv",
        "- sample_comparison_ppg.csv",
        "- compressed_measurements_ppg.csv",
        "- plot_overlay_ppg.png",
        "- plot_metric_trends_ppg.png",
        "",
        "Saran pemakaian di TA:",
        "1. summary_metrics_ppg.csv untuk tabel metrik utama.",
        "2. plot_overlay_ppg.png untuk gambar raw vs rekonstruksi.",
        "3. plot_metric_trends_ppg.png untuk menunjukkan kestabilan performa.",
        "4. Tambahkan keterangan apakah ppg_valid dominan true atau false.",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def main() -> None:
    args = parse_args()
    out_dir = ensure_output_dir(args.out)
    windows = capture(args)

    if not windows:
        print("\n[ERROR] Tidak ada data window yang berhasil direkam.")
        return

    # Rata-rata HR Rekonstruksi (Python)
    valid_py_hrs = [w["hr_python"] for w in windows if w["hr_python"] > 0]
    hr_reconstructed = float(np.mean(valid_py_hrs)) if valid_py_hrs else 0.0

    # Rata-rata HR asli dari ESP32
    valid_esp_hrs = [w["hr"] for w in windows if w["hr"] > 0]
    mean_esp_hr = float(np.mean(valid_esp_hrs)) if valid_esp_hrs else 0.0

    summary_path = write_summary_csv(out_dir, windows)
    sample_path = write_sample_csv(out_dir, windows)
    measurement_path = write_measurement_csv(out_dir, windows)
    overlay_path = plot_overlay(out_dir, windows)
    hr_spo2_path = plot_hr_spo2(out_dir, windows)
    metric_path = plot_metrics(out_dir, windows)
    readme_path = write_readme(out_dir, args, windows)

    print("\n[SELESAI] Semua hasil PPG tersimpan di:")
    print(f"  - {summary_path}")
    print(f"  - {sample_path}")
    print(f"  - {measurement_path}")
    print(f"  - {overlay_path}")
    print(f"  - {hr_spo2_path}")
    print(f"  - {metric_path}")
    print(f"  - {readme_path}")

    print("\n========================================================")
    print("HASIL VERIFIKASI HEART RATE (HR) COMPRESSIVE SENSING")
    print("========================================================")
    print(f"HR Asli (ESP32)          : {mean_esp_hr:.1f} BPM")
    print(f"HR Rekonstruksi (Python) : {hr_reconstructed:.1f} BPM")
    print("========================================================\n")


if __name__ == "__main__":
    main()
