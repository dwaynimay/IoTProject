"""
cs_visualizer_v4.py — Final version, akurasi tinggi dengan M=32

Perubahan dari v3:
  - Alpha LASSO diturunkan ke 0.001 (lebih presisi untuk M=32)
  - Tambah plot koefisien DCT untuk validasi sparsity
  - Tambah indikator visual corr threshold (hijau/kuning/merah)
"""

import json
import collections
import numpy as np
import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
from scipy.fftpack import idct
from sklearn.linear_model import Lasso
import paho.mqtt.client as mqtt

CS_N        = 64
CS_PHI_SEED = 42
LASSO_ALPHA = 0.001   # optimal untuk M=32

MQTT_BROKER = "192.168.1.18"
MQTT_PORT   = 1883
MQTT_TOPIC  = "test/cs_gyro_x"

# ─── Generate Phi ─────────────────────────────────────────────────────────────
def generate_phi(seed, m, n):
    state = np.uint32(seed)
    def lcg():
        nonlocal state
        state = np.uint32(np.uint32(1664525) * state + np.uint32(1013904223))
        return float(np.uint32(state) >> np.uint32(1)) / 2147483647.0
    def gaussian():
        u1 = lcg()
        if u1 < 1e-7: u1 = 1e-7
        u2 = lcg()
        return float(np.sqrt(-2.0 * np.log(u1)) * np.cos(2.0 * np.pi * u2))
    phi = np.zeros((m, n), dtype=np.float64)
    for i in range(m):
        row = np.array([gaussian() for _ in range(n)], dtype=np.float64)
        norm = np.linalg.norm(row)
        if norm > 1e-10:
            row /= (norm * np.sqrt(m))
        phi[i] = row
    return phi

CS_M_CURRENT = None
PHI = None
Psi = idct(np.eye(CS_N), norm='ortho', axis=0)
Theta = None

def init_matrices(m):
    global PHI, Theta, CS_M_CURRENT
    CS_M_CURRENT = m
    import math
    print(f"\n[CS] M={m} | kompresi {m*100//CS_N}%")
    print(f"[CS] Bangkitkan Φ({m}×{CS_N})...", end=" ", flush=True)
    PHI   = generate_phi(CS_PHI_SEED, m, CS_N)
    Theta = PHI @ Psi
    print("OK")
    fig.suptitle(
        f'Compressive Sensing | N={CS_N} M={m} ({m*100//CS_N}%) | α={LASSO_ALPHA}',
        fontsize=11, fontweight='bold')

# ─── State ────────────────────────────────────────────────────────────────────
data_queue    = collections.deque(maxlen=5)
history_corr  = []
history_rmse  = []
window_labels = []
MAX_HIST = 60

# ─── Plot setup ───────────────────────────────────────────────────────────────
plt.ion()
fig = plt.figure(figsize=(12, 13))
gs  = fig.add_gridspec(4, 2, hspace=0.45, wspace=0.3)
ax1     = fig.add_subplot(gs[0, :])    # sinyal asli — full width
ax2     = fig.add_subplot(gs[1, 0])    # measurement y
ax_dct  = fig.add_subplot(gs[1, 1])    # koefisien DCT
ax3     = fig.add_subplot(gs[2, :])    # rekonstruksi — full width
ax4     = fig.add_subplot(gs[3, :])    # tren akurasi
ax4_twin = ax4.twinx()

fig.canvas.manager.set_window_title('CS Rekonstruksi Live')

def corr_color(c):
    if c >= 0.95: return 'green'
    if c >= 0.85: return 'orange'
    return 'red'

def process_and_plot(data):
    global CS_M_CURRENT, history_corr, history_rmse, window_labels

    x_asli     = np.array(data.get("x_asli",     []), dtype=np.float64)
    y_kompresi = np.array(data.get("y_kompresi", []), dtype=np.float64)
    win_num    = int(data.get("win", 0))

    if len(x_asli) != CS_N or len(y_kompresi) == 0:
        return

    m_recv = len(y_kompresi)
    if CS_M_CURRENT is None or CS_M_CURRENT != m_recv:
        init_matrices(m_recv)

    # ── Rekonstruksi LASSO ───────────────────────────────────────────────────
    try:
        lasso = Lasso(alpha=LASSO_ALPHA, max_iter=10000,
                      fit_intercept=False, tol=1e-5)
        lasso.fit(Theta, y_kompresi)
        s_hat = lasso.coef_
        x_hat = Psi @ s_hat
    except Exception as e:
        print(f"[ERROR] {e}")
        return

    rmse = float(np.sqrt(np.mean((x_hat - x_asli)**2)))
    corr = float(np.corrcoef(x_hat, x_asli)[0,1]) if np.std(x_hat) > 0 else 0.0
    max_err = float(np.max(np.abs(x_hat - x_asli)))

    history_corr.append(corr)
    history_rmse.append(rmse)
    window_labels.append(win_num)
    if len(history_corr) > MAX_HIST:
        history_corr.pop(0); history_rmse.pop(0); window_labels.pop(0)

    cc = corr_color(corr)

    # ── Plot 1: sinyal asli ──────────────────────────────────────────────────
    ax1.cla()
    ax1.plot(x_asli, 'g-o', markersize=3, linewidth=1)
    ax1.set_title(f"1. Sinyal Asli x — {CS_N} sampel  [Window #{win_num}]", fontsize=10)
    ax1.set_ylabel("Gyro X (°/s)"); ax1.grid(True, alpha=0.3)

    # ── Plot 2: measurement y ────────────────────────────────────────────────
    ax2.cla()
    ax2.stem(y_kompresi, basefmt=" ", markerfmt='C0o', linefmt='C0-')
    ax2.set_title(f"2. y — {m_recv} nilai ({m_recv*100//CS_N}%)", fontsize=9)
    ax2.set_ylabel("Pengukuran"); ax2.grid(True, alpha=0.3)

    # ── Plot 3: koefisien DCT (sparsity visualization) ───────────────────────
    ax_dct.cla()
    nonzero_mask = np.abs(s_hat) > 1e-4
    colors = ['C1' if nz else 'C0' for nz in nonzero_mask]
    ax_dct.bar(range(CS_N), np.abs(s_hat), color=colors, width=0.8)
    nnz = int(nonzero_mask.sum())
    ax_dct.set_title(f"3. Koef DCT ŝ — {nnz} aktif dari {CS_N}", fontsize=9)
    ax_dct.set_ylabel("|ŝ|"); ax_dct.grid(True, alpha=0.3, axis='y')

    # ── Plot 4: rekonstruksi overlay ─────────────────────────────────────────
    ax3.cla()
    ax3.fill_between(range(CS_N), x_asli, x_hat, alpha=0.12, color='red')
    ax3.plot(x_asli, 'g-',  alpha=0.6, linewidth=1.5, label='Asli')
    ax3.plot(x_hat,  'b-o', markersize=3, linewidth=1.2,
             label=f'Rekonstruksi', color=cc)
    ax3.set_title(
        f"4. Rekonstruksi x̂ vs Asli — corr={corr:.4f}  RMSE={rmse:.1f}",
        fontsize=10, color=cc)
    ax3.set_ylabel("Gyro X (°/s)"); ax3.legend(fontsize=8); ax3.grid(True, alpha=0.3)

    # ── Plot 5: tren akurasi ─────────────────────────────────────────────────
    ax4.cla(); ax4_twin.cla()
    idx = list(range(len(history_corr)))

    if len(idx) > 1:
        # Color tiap titik sesuai akurasi
        for i in range(len(idx)-1):
            c = corr_color(history_corr[i])
            ax4.plot(idx[i:i+2], history_corr[i:i+2], '-', color=c, linewidth=1.5)
        ax4.scatter(idx, history_corr,
                    c=[corr_color(c) for c in history_corr], s=20, zorder=5)
        ax4_twin.plot(idx, history_rmse, 'r--', linewidth=1, alpha=0.6, label='RMSE')

    ax4.axhline(0.95, color='green',  linestyle='--', alpha=0.5, linewidth=1)
    ax4.axhline(0.85, color='orange', linestyle='--', alpha=0.5, linewidth=1)
    ax4.set_ylim(-0.05, 1.05)
    ax4.set_ylabel("Korelasi", fontsize=9)
    ax4_twin.set_ylabel("RMSE (°/s)", color='r', fontsize=9)
    ax4_twin.tick_params(axis='y', labelcolor='r')

    avg = np.mean(history_corr) if history_corr else 0
    ax4.set_title(
        f"5. Tren Akurasi ({len(history_corr)} windows) | avg corr={avg:.3f}",
        fontsize=10, color=corr_color(avg))
    ax4.set_xlabel("Nomor Window", fontsize=9)
    ax4.grid(True, alpha=0.3)

    if window_labels:
        step = max(1, len(window_labels)//8)
        ticks = range(0, len(window_labels), step)
        ax4.set_xticks(list(ticks))
        ax4.set_xticklabels([str(window_labels[t]) for t in ticks], fontsize=7)

    fig.canvas.draw()
    fig.canvas.flush_events()

    status = "✓" if corr >= 0.95 else "~" if corr >= 0.85 else "✗"
    print(f"[Win #{win_num:4d}] M={m_recv} | "
          f"corr={corr:.4f} {status} | RMSE={rmse:5.1f} | max_err={max_err:5.1f}")

# ─── MQTT ─────────────────────────────────────────────────────────────────────
def on_message(client, userdata, msg):
    try:
        data_queue.append(json.loads(msg.payload.decode()))
    except Exception as e:
        print(f"[ERROR] JSON: {e}")

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Terhubung → subscribe {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"[MQTT] Gagal rc={rc}")

print("=" * 55)
print(f"  CS Visualizer v4 | α={LASSO_ALPHA}")
print(f"  N={CS_N} | seed={CS_PHI_SEED}")
print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
print("=" * 55)

mqttClient = mqtt.Client()
mqttClient.on_connect = on_connect
mqttClient.on_message = on_message
try:
    mqttClient.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
except Exception as e:
    print(f"[ERROR] Broker {MQTT_BROKER}: {e}"); exit(1)

mqttClient.loop_start()
print("Menunggu data... (Ctrl+C untuk keluar)\n")

try:
    while True:
        while data_queue:
            process_and_plot(data_queue.popleft())
        plt.pause(0.05)
except KeyboardInterrupt:
    print("\n[INFO] Berhenti.")
finally:
    mqttClient.loop_stop()
    mqttClient.disconnect()
    plt.close('all')