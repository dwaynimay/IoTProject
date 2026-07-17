"""
capture_cs_test.py - Rekam hasil uji Compressive Sensing dari serial ESP32.

Alur kerja:
1. ESP32 menjalankan environment `test_imu_raw_vs_cs`.
2. Script ini membaca JSON serial per-window.
3. Tiap axis direkonstruksi ulang dengan pipeline server.
4. Hasil disimpan ke CSV + grafik PNG agar mudah dipakai di buku TA.

Contoh:
    python -m server.tools.capture_cs_test --port COM5 --windows 10
    python -m server.tools.capture_cs_test --port COM5 --out server/data/cs_ta_run_01
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import serial


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
SERVER_ROOT = PROJECT_ROOT / "server"
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))

from server.cs.hadamard import build_theta, generate_phi, reconstruct  # noqa: E402
from server.core.config import CS_PHI_SEED, OMP_K  # noqa: E402


AXES = ["ax", "ay", "az", "gx", "gy", "gz"]
CS_N = 64
CS_M = 32
PHI = generate_phi(CS_PHI_SEED, CS_M, CS_N)
THETA, PSI = build_theta(PHI, CS_N)


@dataclass
class AxisResult:
    window_idx: int
    axis: str
    mean: float
    rmse: float
    mae: float
    max_abs_error: float
    mape: float
    snr_db: float
    corrcoef: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Tangkap data uji CS dari serial ESP32 lalu simpan CSV + PNG."
    )
    parser.add_argument("--port", required=True, help="Port serial ESP32, contoh COM5")
    parser.add_argument("--baud", type=int, default=115200, help="Baudrate serial")
    parser.add_argument(
        "--windows",
        type=int,
        default=10,
        help="Jumlah window valid yang ingin direkam",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="Batas waktu total perekaman dalam detik",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Folder output. Default: server/data/cs_capture_<timestamp>",
    )
    return parser.parse_args()


def ensure_output_dir(out_dir: Path | None) -> Path:
    if out_dir is None:
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        out_dir = PROJECT_ROOT / "server" / "data" / f"cs_capture_{timestamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir


def safe_float_list(payload: dict, key: str, expected_len: int) -> np.ndarray:
    values = payload.get(key)
    if not isinstance(values, list) or len(values) != expected_len:
        raise ValueError(f"Field '{key}' tidak valid atau panjangnya bukan {expected_len}")
    return np.array(values, dtype=float)


def calc_metrics(x_raw: np.ndarray, x_hat: np.ndarray) -> tuple[float, float, float, float, float, float]:
    error = x_raw - x_hat
    rmse = float(np.sqrt(np.mean(np.square(error))))
    mae = float(np.mean(np.abs(error)))
    max_abs_error = float(np.max(np.abs(error)))

    denom = np.maximum(np.abs(x_raw), 1e-9)
    mape = float(np.mean(np.abs(error) / denom) * 100.0)

    signal_power = float(np.mean(np.square(x_raw)))
    noise_power = float(np.mean(np.square(error)))
    snr_db = 99.0 if noise_power <= 1e-12 else float(10.0 * math.log10(signal_power / noise_power))

    if np.std(x_raw) <= 1e-12 or np.std(x_hat) <= 1e-12:
        corrcoef = 1.0 if np.allclose(x_raw, x_hat) else 0.0
    else:
        corrcoef = float(np.corrcoef(x_raw, x_hat)[0, 1])

    return rmse, mae, max_abs_error, mape, snr_db, corrcoef


def capture_windows(args: argparse.Namespace) -> tuple[list[dict], list[AxisResult]]:
    raw_windows: list[dict] = []
    axis_results: list[AxisResult] = []

    start_time = time.time()
    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        print(f"[INFO] Tersambung ke {args.port} @ {args.baud}")
        print(f"[INFO] Menunggu {args.windows} window valid dari firmware test_imu_raw_vs_cs...")

        while len(raw_windows) < args.windows:
            if time.time() - start_time > args.timeout:
                raise TimeoutError(
                    f"Waktu habis. Hanya menerima {len(raw_windows)} dari {args.windows} window."
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

            if payload.get("type") != "cs_test":
                continue

            window_idx = len(raw_windows) + 1
            enriched: dict[str, object] = {"window_idx": window_idx}
            metrics_text: list[str] = []

            for axis in AXES:
                x_raw = safe_float_list(payload, f"{axis}_raw", CS_N)
                y_cs = safe_float_list(payload, f"{axis}_cs", CS_M)
                mean_val = float(payload.get(f"mean_{axis}", 0.0))

                x_hat_centered = reconstruct(y_cs.tolist(), THETA, PSI, OMP_K)
                x_hat = np.array(x_hat_centered, dtype=float) + mean_val

                rmse, mae, max_abs_error, mape, snr_db, corrcoef = calc_metrics(x_raw, x_hat)

                axis_results.append(
                    AxisResult(
                        window_idx=window_idx,
                        axis=axis,
                        mean=mean_val,
                        rmse=rmse,
                        mae=mae,
                        max_abs_error=max_abs_error,
                        mape=mape,
                        snr_db=snr_db,
                        corrcoef=corrcoef,
                    )
                )

                enriched[f"{axis}_raw"] = x_raw.tolist()
                enriched[f"{axis}_cs"] = y_cs.tolist()
                enriched[f"{axis}_reconstructed"] = x_hat.tolist()
                enriched[f"mean_{axis}"] = mean_val

                metrics_text.append(f"{axis.upper()} RMSE={rmse:.4f}")

            raw_windows.append(enriched)
            print(f"[OK] Window {window_idx}/{args.windows} | " + " | ".join(metrics_text))

    return raw_windows, axis_results


def write_summary_csv(out_dir: Path, axis_results: list[AxisResult]) -> Path:
    path = out_dir / "summary_metrics.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "window_idx",
                "axis",
                "mean",
                "rmse",
                "mae",
                "max_abs_error",
                "mape_percent",
                "snr_db",
                "corrcoef",
            ]
        )
        for item in axis_results:
            writer.writerow(
                [
                    item.window_idx,
                    item.axis,
                    f"{item.mean:.6f}",
                    f"{item.rmse:.6f}",
                    f"{item.mae:.6f}",
                    f"{item.max_abs_error:.6f}",
                    f"{item.mape:.6f}",
                    f"{item.snr_db:.6f}",
                    f"{item.corrcoef:.6f}",
                ]
            )
    return path


def write_samples_csv(out_dir: Path, windows: list[dict]) -> Path:
    path = out_dir / "sample_comparison.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "window_idx",
                "axis",
                "sample_idx",
                "raw_value",
                "reconstructed_value",
                "abs_error",
            ]
        )
        for window in windows:
            for axis in AXES:
                raw_values = window[f"{axis}_raw"]
                rec_values = window[f"{axis}_reconstructed"]
                for sample_idx, (raw_value, rec_value) in enumerate(zip(raw_values, rec_values)):
                    writer.writerow(
                        [
                            window["window_idx"],
                            axis,
                            sample_idx,
                            f"{raw_value:.6f}",
                            f"{rec_value:.6f}",
                            f"{abs(raw_value - rec_value):.6f}",
                        ]
                    )
    return path


def write_measurements_csv(out_dir: Path, windows: list[dict]) -> Path:
    path = out_dir / "compressed_measurements.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["window_idx", "axis", "measurement_idx", "measurement_value"])
        for window in windows:
            for axis in AXES:
                for measurement_idx, value in enumerate(window[f"{axis}_cs"]):
                    writer.writerow(
                        [
                            window["window_idx"],
                            axis,
                            measurement_idx,
                            f"{value:.6f}",
                        ]
                    )
    return path


def write_axis_mean_csv(out_dir: Path, axis_results: list[AxisResult]) -> Path:
    path = out_dir / "summary_axis_mean.csv"
    grouped: dict[str, list[AxisResult]] = {axis: [] for axis in AXES}
    for item in axis_results:
        grouped[item.axis].append(item)

    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["axis", "mean_rmse", "mean_mae", "mean_mape_percent", "mean_snr_db", "mean_corrcoef"])
        for axis in AXES:
            items = grouped[axis]
            writer.writerow(
                [
                    axis,
                    f"{np.mean([x.rmse for x in items]):.6f}",
                    f"{np.mean([x.mae for x in items]):.6f}",
                    f"{np.mean([x.mape for x in items]):.6f}",
                    f"{np.mean([x.snr_db for x in items]):.6f}",
                    f"{np.mean([x.corrcoef for x in items]):.6f}",
                ]
            )
    return path


def plot_window_overlay(out_dir: Path, windows: list[dict]) -> Path:
    fig, axes = plt.subplots(3, 2, figsize=(16, 10), sharex=True)
    fig.suptitle("Perbandingan Raw vs Hasil Rekonstruksi CS", fontsize=16)

    for index, axis_name in enumerate(AXES):
        ax = axes[index // 2][index % 2]
        for window in windows:
            x_axis = np.arange(CS_N)
            ax.plot(x_axis, window[f"{axis_name}_raw"], alpha=0.35, linewidth=1.0, color="#1f77b4")
            ax.plot(x_axis, window[f"{axis_name}_reconstructed"], alpha=0.35, linewidth=1.0, linestyle="--", color="#d62728")
        ax.set_title(axis_name.upper())
        ax.set_xlabel("Indeks Sampel")
        ax.set_ylabel("Nilai")
        ax.grid(True, linestyle=":", alpha=0.4)

    handles = [
        plt.Line2D([0], [0], color="#1f77b4", linewidth=2, label="Raw"),
        plt.Line2D([0], [0], color="#d62728", linewidth=2, linestyle="--", label="Rekonstruksi"),
    ]
    fig.legend(handles=handles, loc="upper center", ncol=2)
    fig.tight_layout(rect=[0, 0, 1, 0.95])

    path = out_dir / "plot_overlay_all_windows.png"
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_combined_overlay(out_dir: Path, windows: list[dict]) -> Path:
    fig, ax = plt.subplots(figsize=(18, 8))
    colors = {
        "ax": "#1f77b4",
        "ay": "#2ca02c",
        "az": "#ff7f0e",
        "gx": "#d62728",
        "gy": "#9467bd",
        "gz": "#17becf",
    }

    for axis_name in AXES:
        raw_series: list[float] = []
        rec_series: list[float] = []
        for window in windows:
            raw_series.extend(window[f"{axis_name}_raw"])
            rec_series.extend(window[f"{axis_name}_reconstructed"])

        x_axis = np.arange(len(raw_series))
        # Raw dibuat putus-putus agar jelas itu acuan pengukuran asli.
        ax.plot(
            x_axis,
            raw_series,
            linewidth=1.0,
            linestyle="--",
            alpha=0.55,
            color=colors[axis_name],
            label=f"{axis_name.upper()} Raw",
        )
        # Rekonstruksi dibuat garis utuh agar pembaca mudah melihat hasil akhirnya.
        ax.plot(
            x_axis,
            rec_series,
            linewidth=1.3,
            linestyle="-",
            alpha=0.9,
            color=colors[axis_name],
            label=f"{axis_name.upper()} Rekonstruksi",
        )

    ax.set_title("Overlay Gabungan Semua Axis: Raw vs Rekonstruksi")
    ax.set_xlabel("Indeks Sampel Gabungan")
    ax.set_ylabel("Nilai Sensor")
    ax.grid(True, linestyle=":", alpha=0.4)
    ax.legend(loc="upper right", ncol=2, fontsize=9)
    fig.tight_layout()

    path = out_dir / "plot_overlay_combined.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_overlay_per_axis(out_dir: Path, windows: list[dict]) -> list[Path]:
    colors = {
        "ax": "#1f77b4",
        "ay": "#2ca02c",
        "az": "#ff7f0e",
        "gx": "#d62728",
        "gy": "#9467bd",
        "gz": "#17becf",
    }
    output_paths: list[Path] = []

    for axis_name in AXES:
        fig, ax = plt.subplots(figsize=(16, 5))
        raw_series: list[float] = []
        rec_series: list[float] = []
        for window in windows:
            raw_series.extend(window[f"{axis_name}_raw"])
            rec_series.extend(window[f"{axis_name}_reconstructed"])

        x_axis = np.arange(len(raw_series))
        ax.plot(
            x_axis,
            raw_series,
            linewidth=1.1,
            linestyle="--",
            alpha=0.65,
            color=colors[axis_name],
            label="Data Asli",
        )
        ax.plot(
            x_axis,
            rec_series,
            linewidth=1.6,
            linestyle="-",
            alpha=0.95,
            color="#111111",
            label="Data Rekonstruksi",
        )
        ax.set_title(f"Overlay {axis_name.upper()}: Data Asli vs Rekonstruksi")
        ax.set_xlabel("Indeks Sampel Gabungan")
        ax.set_ylabel("Nilai Sensor")
        ax.grid(True, linestyle=":", alpha=0.4)
        ax.legend(loc="best")
        fig.tight_layout()

        path = out_dir / f"plot_overlay_{axis_name}.png"
        fig.savefig(path, dpi=220, bbox_inches="tight")
        plt.close(fig)
        output_paths.append(path)

    return output_paths


def plot_metric_trends(out_dir: Path, axis_results: list[AxisResult]) -> Path:
    fig, axes = plt.subplots(2, 2, figsize=(16, 10), sharex=True)
    metric_specs = [
        ("rmse", "RMSE"),
        ("mae", "MAE"),
        ("snr_db", "SNR (dB)"),
        ("corrcoef", "Korelasi"),
    ]

    for plot_idx, (metric_key, metric_label) in enumerate(metric_specs):
        ax = axes[plot_idx // 2][plot_idx % 2]
        for axis_name in AXES:
            filtered = [item for item in axis_results if item.axis == axis_name]
            x_vals = [item.window_idx for item in filtered]
            y_vals = [getattr(item, metric_key) for item in filtered]
            ax.plot(x_vals, y_vals, marker="o", linewidth=1.5, label=axis_name.upper())
        ax.set_title(metric_label)
        ax.set_xlabel("Window")
        ax.grid(True, linestyle=":", alpha=0.4)

    axes[0][0].legend(loc="best", ncol=3)
    fig.suptitle("Tren Metrik Kualitas Rekonstruksi per Window", fontsize=16)
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    path = out_dir / "plot_metric_trends.png"
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    return path


def write_readme(out_dir: Path, args: argparse.Namespace, windows: list[dict], axis_results: list[AxisResult]) -> Path:
    axis_means: dict[str, dict[str, float]] = {}
    for axis_name in AXES:
        filtered = [item for item in axis_results if item.axis == axis_name]
        axis_means[axis_name] = {
            "rmse": float(np.mean([item.rmse for item in filtered])),
            "mae": float(np.mean([item.mae for item in filtered])),
            "snr_db": float(np.mean([item.snr_db for item in filtered])),
            "corrcoef": float(np.mean([item.corrcoef for item in filtered])),
        }

    path = out_dir / "README_HASIL.txt"
    lines = [
        "HASIL UJI COMPRESSIVE SENSING",
        "==============================",
        f"Port serial     : {args.port}",
        f"Baudrate        : {args.baud}",
        f"Jumlah window   : {len(windows)}",
        f"Panjang window  : {CS_N} sampel",
        f"Jumlah y CS     : {CS_M} pengukuran",
        "",
        "File output utama:",
        "- summary_metrics.csv      : metrik per-window per-axis",
        "- summary_axis_mean.csv    : rata-rata metrik per-axis",
        "- sample_comparison.csv    : data raw vs rekonstruksi per sampel",
        "- compressed_measurements.csv : data pengukuran kompresi (y)",
        "- plot_overlay_all_windows.png : overlay multi-panel per axis",
        "- plot_overlay_combined.png : overlay gabungan semua axis",
        "- plot_overlay_ax.png ... plot_overlay_gz.png : overlay terpisah tiap axis",
        "- plot_metric_trends.png   : grafik tren RMSE/MAE/SNR/korelasi",
        "",
        "Ringkasan rata-rata per-axis:",
    ]

    for axis_name in AXES:
        axis_summary = axis_means[axis_name]
        lines.append(
            f"- {axis_name.upper()}: RMSE={axis_summary['rmse']:.4f}, "
            f"MAE={axis_summary['mae']:.4f}, "
            f"SNR={axis_summary['snr_db']:.2f} dB, "
            f"Korelasi={axis_summary['corrcoef']:.4f}"
        )

    lines.extend(
        [
            "",
            "Saran pemakaian di buku TA:",
            "1. Pakai summary_axis_mean.csv untuk tabel ringkasan utama.",
            "2. Pakai summary_metrics.csv jika ingin tabel detail per-window.",
            "3. Pakai plot_overlay_combined.png untuk satu gambar ringkasan keseluruhan.",
            "4. Pakai plot_overlay_ax.png sampai plot_overlay_gz.png untuk analisis per-axis.",
            "5. Pakai plot_metric_trends.png untuk analisis kestabilan performa CS.",
        ]
    )

    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def main() -> None:
    args = parse_args()
    out_dir = ensure_output_dir(args.out)

    windows, axis_results = capture_windows(args)

    summary_path = write_summary_csv(out_dir, axis_results)
    mean_path = write_axis_mean_csv(out_dir, axis_results)
    sample_path = write_samples_csv(out_dir, windows)
    measurement_path = write_measurements_csv(out_dir, windows)
    overlay_plot_path = plot_window_overlay(out_dir, windows)
    combined_overlay_path = plot_combined_overlay(out_dir, windows)
    per_axis_overlay_paths = plot_overlay_per_axis(out_dir, windows)
    metric_plot_path = plot_metric_trends(out_dir, axis_results)
    readme_path = write_readme(out_dir, args, windows, axis_results)

    print("\n[SELESAI] Semua hasil tersimpan di:")
    print(f"  - {summary_path}")
    print(f"  - {mean_path}")
    print(f"  - {sample_path}")
    print(f"  - {measurement_path}")
    print(f"  - {overlay_plot_path}")
    print(f"  - {combined_overlay_path}")
    for overlay_path in per_axis_overlay_paths:
        print(f"  - {overlay_path}")
    print(f"  - {metric_plot_path}")
    print(f"  - {readme_path}")


if __name__ == "__main__":
    main()
