"""
server/apps/test_single_signal.py
Pengganti tes.py (cs_visualizer_v4.py)

Test rekonstruksi 1 sinyal (default: gyro_x) dengan plot detail.
Berguna untuk tuning LASSO_ALPHA dan verifikasi Φ identik dengan firmware.

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
    CS_N, CS_M, CS_PHI_SEED, LASSO_ALPHA,
    MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, TOPIC_BASE,
)
from server.core.cs_utils import THETA, PSI, reconstruct

# ── Konfigurasi test ──────────────────────────────────────────────────────────
NODE_ID    = 1
SIGNAL     = "gx"           # sinyal yang ditest: ax/ay/az/gx/gy/gz/ir
MQTT_TOPIC = f"{TOPIC_BASE}/node_{NODE_ID}/cs_{SIGNAL}"
MAX_HIST   = 60

# ── State ─────────────────────────────────────────────────────────────────────
data_queue    = collections.deque(maxlen=5)
history_corr  = []
history_rmse  = []
window_labels = []


def _corr_color(c: float) -> str:
    if c >= 0.95: return 'green'
    if c >= 0.85: return 'orange'
    return 'red'


# ── Plot setup ────────────────────────────────────────────────────────────────
plt.ion()
fig = plt.figure(figsize=(12, 10))
gs  = fig.add_gridspec(3, 2, hspace=0.45, wspace=0.3)
ax_y    = fig.add_subplot(gs[0, 0])   # measurement y
ax_dct  = fig.add_subplot(gs[0, 1])   # koefisien DCT
ax_rec  = fig.add_subplot(gs[1, :])   # rekonstruksi full width
ax_acc  = fig.add_subplot(gs[2, :])   # tren akurasi
ax_acc2 = ax_acc.twinx()

fig.canvas.manager.set_window_title(
    f'CS Test Single Signal | {SIGNAL} | Node {NODE_ID} | alpha={LASSO_ALPHA}')
fig.suptitle(
    f'CS Test — Sinyal Nyata (tanpa ground truth) | N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | alpha={LASSO_ALPHA}',
    fontsize=10, fontweight='bold')


def _process(data: dict):
    global history_corr, history_rmse, window_labels

    y_raw   = data.get("y", [])
    win_num = int(data.get("win", 0)) if "win" in data else len(history_corr) + 1
    ts      = data.get("ts", 0)
    finger  = data.get("finger", False)

    if len(y_raw) != CS_M:
        print(f"[WARN] y len={len(y_raw)}, expected {CS_M}")
        return

    y_arr = np.array(y_raw, dtype=np.float64)

    # Rekonstruksi
    try:
        x_hat = reconstruct(y_arr, THETA, PSI, LASSO_ALPHA)
    except Exception as e:
        print(f"[ERROR] Rekonstruksi: {e}")
        return

    # "x_asli" tidak tersedia di mode deploy (hanya y dikirim dari sensor).
    # Untuk test accuracy kita pakai x_hat saja dan tampilkan statistik dasar.
    rmse_proxy = float(np.std(x_hat))  # proxy: std deviation rekonstruksi
    nnz        = int(np.sum(np.abs(x_hat) > 1e-4 * np.max(np.abs(x_hat) + 1e-9)))

    history_rmse.append(rmse_proxy)
    # Korelasi tidak bisa dihitung tanpa ground truth — tampilkan sparsity proxy
    sparsity = 1.0 - nnz / CS_N
    history_corr.append(sparsity)
    window_labels.append(win_num)
    if len(history_corr) > MAX_HIST:
        history_corr.pop(0)
        history_rmse.pop(0)
        window_labels.pop(0)

    # ── Plot measurement y ───────────────────────────────────────────────────
    ax_y.cla()
    ax_y.stem(y_arr, basefmt=" ", markerfmt='C0o', linefmt='C0-')
    ax_y.set_title(f"y — {CS_M} nilai ({CS_M*100//CS_N}%)", fontsize=9)
    ax_y.set_ylabel("Pengukuran")
    ax_y.grid(True, alpha=0.3)

    # ── Plot koefisien DCT ────────────────────────────────────────────────────
    s_hat       = PSI.T @ x_hat  # approx sparse representation
    nz_mask     = np.abs(s_hat) > 1e-4
    bar_colors  = ['C1' if v else 'C0' for v in nz_mask]
    ax_dct.cla()
    ax_dct.bar(range(CS_N), np.abs(s_hat), color=bar_colors, width=0.8)
    ax_dct.set_title(f"Koef DCT | {nnz} aktif dari {CS_N}", fontsize=9)
    ax_dct.set_ylabel("|ŝ|")
    ax_dct.grid(True, alpha=0.3, axis='y')

    # ── Plot rekonstruksi ─────────────────────────────────────────────────────
    ax_rec.cla()
    ax_rec.plot(x_hat, 'b-o', markersize=3, linewidth=1.2,
                label=f'Rekonstruksi x̂  [finger={"Y" if finger else "N"}]')
    ax_rec.set_title(
        f"Rekonstruksi {SIGNAL} | Win #{win_num} | ts={ts}ms | std={rmse_proxy:.3f}",
        fontsize=10)
    ax_rec.set_ylabel(SIGNAL)
    ax_rec.legend(fontsize=8)
    ax_rec.grid(True, alpha=0.3)

    # ── Plot tren sparsity / std ──────────────────────────────────────────────
    ax_acc.cla(); ax_acc2.cla()
    idx = list(range(len(history_corr)))
    if len(idx) > 1:
        for i in range(len(idx) - 1):
            c = _corr_color(history_corr[i])
            ax_acc.plot(idx[i:i+2], history_corr[i:i+2], '-', color=c, linewidth=1.5)
        ax_acc.scatter(idx, history_corr,
                       c=[_corr_color(c) for c in history_corr], s=20, zorder=5)
        ax_acc2.plot(idx, history_rmse, 'r--', linewidth=1, alpha=0.6, label='Std dev')

    ax_acc.axhline(0.7, color='green',  linestyle='--', alpha=0.5, linewidth=1)
    ax_acc.axhline(0.5, color='orange', linestyle='--', alpha=0.5, linewidth=1)
    ax_acc.set_ylim(0, 1.05)
    ax_acc.set_ylabel("Sparsity (1 - nnz/N)", fontsize=9)
    ax_acc2.set_ylabel("Std dev rekonstruksi", color='r', fontsize=9)
    ax_acc2.tick_params(axis='y', labelcolor='r')
    avg = np.mean(history_corr) if history_corr else 0
    ax_acc.set_title(f"Tren Sparsity ({len(history_corr)} windows) | avg={avg:.3f} "
                     f"[Note: corr tidak tersedia tanpa ground truth]",
                     fontsize=9, color=_corr_color(avg))
    ax_acc.set_xlabel("Nomor Window", fontsize=9)
    ax_acc.set_ylabel("Sparsity (1 - nnz/N)", fontsize=9)
    ax_acc.grid(True, alpha=0.3)

    fig.canvas.draw()
    fig.canvas.flush_events()

    print(f"[Win #{win_num:4d}] M={CS_M} | nnz={nnz:2d} | "
          f"sparsity={sparsity:.3f} | std={rmse_proxy:.3f} | finger={'Y' if finger else 'N'}")


# ── MQTT ──────────────────────────────────────────────────────────────────────
def _on_message(client, userdata, msg):
    try:
        data_queue.append(json.loads(msg.payload.decode()))
    except Exception as e:
        print(f"[ERROR] JSON: {e}")

def _on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung → subscribe {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"[MQTT] Gagal rc={rc_val}")


if __name__ == "__main__":
    print("=" * 55)
    print(f"  CS Test Single Signal | {SIGNAL} | Node {NODE_ID}")
    print(f"  N={CS_N} M={CS_M} | alpha={LASSO_ALPHA}")
    print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  Topic: {MQTT_TOPIC}")
    print("=" * 55)

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
    print("Menunggu data... (Ctrl+C untuk keluar)\n")

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
