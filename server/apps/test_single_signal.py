"""
server/apps/test_single_signal.py

Test rekonstruksi 1 sinyal dengan metrik akurasi yang bermakna.
Berguna untuk tuning OMP_K dan verifikasi Φ identik dengan firmware.

Metrik yang ditampilkan (tanpa ground truth x_asli):
  - Measurement residual RMSE : ||y - Φ·x̂||₂ / ||y||₂
      → Seberapa baik x̂ "menjelaskan" measurement y yang diterima.
        Nilainya 0.0 = sempurna, > 0.3 = rekonstruksi buruk.
  - Sparsity koefisien OMP    : K aktif / N total
      → Berapa komponen frekuensi yang dipakai OMP.
        Harusnya jauh lebih kecil dari N (sparse).

Jalankan dari root project:
    python -m server.apps.test_single_signal
"""

import json
import warnings
import collections
import numpy as np

warnings.filterwarnings("ignore", category=UserWarning)
warnings.filterwarnings("ignore", category=RuntimeWarning)

import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt

import paho.mqtt.client as mqtt
try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

from server.core.config import (
    CS_N, CS_M, CS_PHI_SEED, OMP_K,
    MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, TOPIC_BASE,
)
from server.core.cs_utils import PHI, THETA_REAL, PSI_C, reconstruct

# ── Konfigurasi test ──────────────────────────────────────────────────────────
NODE_ID    = 1
SIGNAL     = "gx"      # sinyal yang ditest: ax/ay/az/gx/gy/gz/ir
MQTT_TOPIC = f"{TOPIC_BASE}/node_{NODE_ID}/cs_{SIGNAL}"
MAX_HIST   = 60

# ── State ─────────────────────────────────────────────────────────────────────
data_queue       = collections.deque(maxlen=5)
history_residual = []   # measurement residual RMSE (metrik utama akurasi)
history_sparsity = []   # K aktif / N (berapa koef DCT yang dipakai OMP)
window_labels    = []


def _quality_color(residual: float) -> str:
    """Warna berdasarkan measurement residual — makin kecil makin baik."""
    if residual < 0.10: return 'green'
    if residual < 0.25: return 'orange'
    return 'red'


# ── Plot setup ────────────────────────────────────────────────────────────────
plt.ion()
fig = plt.figure(figsize=(13, 11))
gs  = fig.add_gridspec(3, 2, hspace=0.50, wspace=0.32)

ax_y      = fig.add_subplot(gs[0, 0])   # measurement y asli vs y_hat
ax_sparse = fig.add_subplot(gs[0, 1])   # koefisien OMP (sparse di DCT)
ax_rec    = fig.add_subplot(gs[1, :])   # sinyal rekonstruksi x̂
ax_trend  = fig.add_subplot(gs[2, :])   # tren residual RMSE + sparsity
ax_trend2 = ax_trend.twinx()

fig.canvas.manager.set_window_title(
    f'CS Test | {SIGNAL} | Node {NODE_ID} | K={OMP_K}')
fig.suptitle(
    f'CS Test — Hadamard-Gaussian + DCT + OMP | '
    f'N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | K={OMP_K}',
    fontsize=10, fontweight='bold')


# ── Helper: ambil koefisien DCT sparse dari OMP ──────────────────────────────
def _get_omp_coeffs(y_arr: np.ndarray) -> tuple[np.ndarray, list[int]]:
    """
    Jalankan OMP untuk mendapatkan koefisien DCT dan support set (index aktif).

    cs_utils sekarang pakai DCT basis (murni real):
      - s_hat panjang N (bukan 2N seperti versi FFT lama)
      - tidak ada split real/imag — langsung |s_hat[i]| untuk magnitude

    Returns:
        s_hat   : np.ndarray (N,)   koefisien DCT
        support : list[int]         index koefisien aktif (|s| > threshold)
    """
    from server.core.cs_utils import omp
    s_hat   = omp(y_arr, THETA_REAL, OMP_K)          # (N,) — bukan (2N,)
    support = [i for i in range(CS_N) if abs(s_hat[i]) > 1e-8]
    return s_hat, support


def _process(data: dict):
    global history_residual, history_sparsity, window_labels

    y_raw   = data.get("y", [])
    win_num = int(data.get("ts", len(history_residual) + 1))
    ts      = data.get("ts", 0)
    finger  = data.get("finger", False)

    if len(y_raw) != CS_M:
        print(f"[WARN] y len={len(y_raw)}, expected {CS_M}")
        return

    y_arr = np.array(y_raw, dtype=np.float64)

    # ── Rekonstruksi ─────────────────────────────────────────────────────────
    try:
        x_hat          = reconstruct(y_arr, THETA_REAL, PSI_C, OMP_K)
        s_hat, support = _get_omp_coeffs(y_arr)
    except Exception as e:
        print(f"[ERROR] Rekonstruksi: {e}")
        return

    # ── Metrik akurasi tanpa ground truth ────────────────────────────────────
    #
    # 1. Measurement residual (metrik utama):
    #    y_hat = Φ · x̂  (re-encode hasil rekonstruksi)
    #    residual = ||y - y_hat||₂ / ||y||₂   (normalized)
    #    Interpretasi:
    #      < 0.10 → rekonstruksi sangat baik  (hijau)
    #      0.10–0.25 → cukup baik             (oranye)
    #      > 0.25 → rekonstruksi buruk        (merah) → naikkan K atau cek Φ
    #
    y_hat    = PHI @ x_hat                           # re-encode (M,)
    y_norm   = np.linalg.norm(y_arr)
    residual = float(np.linalg.norm(y_arr - y_hat) / (y_norm + 1e-9))

    # 2. Sparsity OMP: berapa koef aktif dari N yang tersedia
    k_active = len(support)
    sparsity = k_active / CS_N                       # 0 = sangat sparse

    # 3. Std rekonstruksi (gambaran amplitudo sinyal)
    std_xhat = float(np.std(x_hat))

    history_residual.append(residual)
    history_sparsity.append(sparsity)
    window_labels.append(win_num)
    if len(history_residual) > MAX_HIST:
        history_residual.pop(0)
        history_sparsity.pop(0)
        window_labels.pop(0)

    qcolor = _quality_color(residual)

    # ── Plot 1: y asli vs y_hat ───────────────────────────────────────────────
    ax_y.cla()
    ax_y.plot(y_arr, 'C0o-', markersize=4, linewidth=1.2, label='y (terima)')
    ax_y.plot(y_hat, 'C3o--', markersize=3, linewidth=1.0, alpha=0.8,
              label='ŷ = Φ·x̂ (re-encode)')
    ax_y.set_title(
        f"y vs ŷ | residual={residual:.4f} "
        f"({'BAIK' if residual < 0.10 else 'OK' if residual < 0.25 else 'BURUK'})",
        fontsize=9, color=qcolor)
    ax_y.set_ylabel("Nilai pengukuran")
    ax_y.legend(fontsize=7, loc='upper right')
    ax_y.grid(True, alpha=0.3)

    # ── Plot 2: koefisien DCT dari OMP ───────────────────────────────────────
    # s_hat panjang N (DCT, murni real) — magnitude = |s_hat[i]|
    s_mag      = np.abs(s_hat)                        # (N,) — tidak perlu sqrt(r²+i²)
    bar_colors = ['C1' if i in support else 'C0' for i in range(CS_N)]
    ax_sparse.cla()
    ax_sparse.bar(range(CS_N), s_mag, color=bar_colors, width=0.8)
    ax_sparse.set_title(
        f"Koef DCT OMP | {k_active} aktif (K={OMP_K}) dari N={CS_N}",
        fontsize=9)
    ax_sparse.set_ylabel("|ŝ| magnitude")
    ax_sparse.grid(True, alpha=0.3, axis='y')

    # ── Plot 3: sinyal rekonstruksi x̂ ────────────────────────────────────────
    ax_rec.cla()
    ax_rec.plot(x_hat, 'b-o', markersize=3, linewidth=1.2,
                color=qcolor,
                label=f'x̂ rekonstruksi  [finger={"Y" if finger else "N"}]')
    ax_rec.set_title(
        f"Rekonstruksi {SIGNAL} | Win #{len(history_residual)} | "
        f"ts={ts}ms | std={std_xhat:.3f} | residual={residual:.4f}",
        fontsize=10, color=qcolor)
    ax_rec.set_ylabel(f"{SIGNAL}")
    ax_rec.legend(fontsize=8)
    ax_rec.grid(True, alpha=0.3)

    # ── Plot 4: tren residual + sparsity ──────────────────────────────────────
    ax_trend.cla()
    ax_trend2.cla()
    idx = list(range(len(history_residual)))

    if len(idx) > 1:
        for i in range(len(idx) - 1):
            c = _quality_color(history_residual[i])
            ax_trend.plot(idx[i:i+2], history_residual[i:i+2],
                          '-', color=c, linewidth=1.5)
        ax_trend.scatter(idx, history_residual,
                         c=[_quality_color(r) for r in history_residual],
                         s=20, zorder=5)
        ax_trend2.plot(idx, history_sparsity, 'b--',
                       linewidth=1, alpha=0.5, label='Sparsity (K aktif/N)')

    ax_trend.axhline(0.10, color='green',  linestyle='--', alpha=0.6,
                     linewidth=0.8, label='target < 0.10')
    ax_trend.axhline(0.25, color='orange', linestyle='--', alpha=0.6,
                     linewidth=0.8, label='batas 0.25')

    ax_trend.set_ylim(0, max(0.5, max(history_residual) * 1.1) if history_residual else 0.5)
    ax_trend2.set_ylim(0, 1.05)

    avg_res = np.mean(history_residual) if history_residual else 0
    ax_trend.set_title(
        f"Tren Akurasi ({len(history_residual)} windows) | "
        f"avg residual={avg_res:.4f} "
        f"({'BAIK' if avg_res < 0.10 else 'OK' if avg_res < 0.25 else 'BURUK — naikkan K'})",
        fontsize=9, color=_quality_color(avg_res))
    ax_trend.set_xlabel("Nomor Window", fontsize=9)
    ax_trend.set_ylabel("Measurement Residual (↓ lebih baik)", fontsize=9)
    ax_trend2.set_ylabel("Sparsity (K aktif / N)", color='blue', fontsize=9)
    ax_trend2.tick_params(axis='y', labelcolor='blue')
    ax_trend.legend(fontsize=7, loc='upper right')
    ax_trend.grid(True, alpha=0.3)

    fig.canvas.draw()
    fig.canvas.flush_events()

    # ── Print ke terminal ─────────────────────────────────────────────────────
    status = 'BAIK ' if residual < 0.10 else 'OK   ' if residual < 0.25 else 'BURUK'
    print(f"[Win #{len(history_residual):4d}] {status} | "
          f"residual={residual:.4f} | "
          f"K_aktif={k_active:2d}/{OMP_K} | "
          f"std={std_xhat:.3f} | "
          f"finger={'Y' if finger else 'N'}")


# ── MQTT ──────────────────────────────────────────────────────────────────────
def _on_message(client, userdata, msg):
    try:
        data_queue.append(json.loads(msg.payload.decode()))
    except Exception as e:
        print(f"[ERROR] JSON: {e}")

def _on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung → {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"[MQTT] Gagal rc={rc_val}")


# ── Main ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("=" * 60)
    print(f"  CS Test Single Signal | {SIGNAL} | Node {NODE_ID}")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | OMP K={OMP_K}")
    print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  Topic : {MQTT_TOPIC}")
    print(f"  Metrik: measurement residual ||y - Φ·x̂|| / ||y||")
    print(f"  Target: residual < 0.10 (hijau) → rekonstruksi baik")
    print("=" * 60)

    if _PAHO_V2:
        mqttClient = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    else:
        mqttClient = mqtt.Client()

    mqttClient.on_connect = _on_connect
    mqttClient.on_message = _on_message

    try:
        mqttClient.connect(MQTT_BROKER, MQTT_PORT, keepalive=MQTT_KEEPALIVE)
    except Exception as e:
        print(f"[ERROR] Broker {MQTT_BROKER}: {e}")
        exit(1)

    mqttClient.loop_start()
    print("\nMenunggu data... (Ctrl+C untuk keluar)\n")
    print(f"{'Win':>6} | {'Status':6} | {'Residual':>10} | {'K aktif':>8} | {'Std':>7} | Finger")
    print("-" * 60)

    try:
        while True:
            while data_queue:
                _process(data_queue.popleft())
            plt.pause(0.05)
    except KeyboardInterrupt:
        print("\n[INFO] Berhenti.")
    finally:
        mqttClient.loop_stop()
        mqttClient.disconnect()
        plt.close('all')