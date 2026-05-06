"""
cs_live_visualizer.py — Visualisasi live hasil rekonstruksi CS sistem nyata

CHANGELOG v2:
  - Hapus emoji (🫰 ✋) → ganti teks ASCII agar tidak warning di Windows
  - Suppress RuntimeWarning overflow LCG (by design, uint32 wrap)
  - Suppress UserWarning glyph missing font
  - Update paho-mqtt ke CallbackAPIVersion v2
  - Optimasi main loop: batch proses window yang menumpuk, plot hanya update
    jika ada data baru (skip plt.pause yang blocking saat queue kosong)
  - window_queue maxlen dinaikkan 3 → 10 agar tidak drop window saat lag plot

Cara pakai:
  Terminal 1: python cs_reconstruct_server.py
  Terminal 2: python cs_live_visualizer.py

Instalasi: pip install paho-mqtt numpy scikit-learn scipy matplotlib
"""

import json
import warnings
import collections
import threading
import numpy as np

# Suppress semua UserWarning dari matplotlib (glyph missing, dll)
# dan RuntimeWarning dari numpy (overflow uint32 di LCG — by design)
warnings.filterwarnings("ignore", category=UserWarning)
warnings.filterwarnings("ignore", category=RuntimeWarning)

import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from scipy.fftpack import idct
from sklearn.linear_model import Lasso
import paho.mqtt.client as mqtt
try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

# ─── Parameter — HARUS sama dengan CS_Sensor.h ───────────────────────────────
CS_N        = 64
CS_M        = 32
CS_PHI_SEED = 42
LASSO_ALPHA = 0.001

MQTT_BROKER = "192.168.1.18"
MQTT_PORT   = 1883
TOPIC_BASE  = "health_monitor"
NODE_ID     = 1       # node yang ingin divisualisasi

SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz", "ir"]
UNITS   = {"ax":"m/s²","ay":"m/s²","az":"m/s²",
           "gx":"deg/s","gy":"deg/s","gz":"deg/s",
           "ir":"ADC"}

# ─── Generate Phi — identik dengan server & ESP32 ────────────────────────────
def generate_phi(seed, m, n):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
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

print(f"Membangkitkan Phi (M={CS_M}, N={CS_N})...", end=" ", flush=True)
PHI   = generate_phi(CS_PHI_SEED, CS_M, CS_N)
Psi   = idct(np.eye(CS_N), norm='ortho', axis=0)
Theta = PHI @ Psi
print("OK")

def reconstruct(y_list):
    y = np.array(y_list, dtype=np.float64)
    lasso = Lasso(alpha=LASSO_ALPHA, max_iter=5000,
                  fit_intercept=False, tol=1e-5)
    lasso.fit(Theta, y)
    return Psi @ lasso.coef_

# ─── Buffer MQTT ──────────────────────────────────────────────────────────────
_buf_lock   = threading.Lock()
_signal_buf = {}

# maxlen=10: bisa tampung ~10 window yang menumpuk sebelum plot catch up
# Sebelumnya maxlen=3 → window #10,#11 di-drop saat plot lag
window_queue = collections.deque(maxlen=10)

HISTORY_WINDOWS = 5
history      = {s: np.zeros(CS_N * HISTORY_WINDOWS) for s in SIGNALS}
MAX_HIST     = 60
hr_history   = []
corr_history = []

meta = {"win": 0}

# ─── MQTT callbacks ───────────────────────────────────────────────────────────
def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except Exception:
        return

    parts = msg.topic.split("/")
    if len(parts) < 3:
        return
    try:
        node_id = int(parts[1].split("_")[1])
    except (IndexError, ValueError):
        return
    if node_id != NODE_ID:
        return

    signal = parts[2].replace("cs_", "")
    if signal not in SIGNALS:
        return

    with _buf_lock:
        _signal_buf[signal] = payload

        if all(s in _signal_buf for s in SIGNALS):
            buf_copy = dict(_signal_buf)
            _signal_buf.clear()

            results = {}
            for sig in SIGNALS:
                y = buf_copy[sig].get("y", [])
                if len(y) == CS_M:
                    results[sig] = reconstruct(y)

            ir_meta  = buf_copy.get("ir", {})
            meta["win"] += 1
            win_data = {
                "win"   : meta["win"],
                "hr"    : ir_meta.get("hr", 0),
                "finger": ir_meta.get("finger", False),
                "ts"    : buf_copy["ax"].get("ts", 0),
                "data"  : results,
            }
            window_queue.append(win_data)

def on_connect(client, userdata, *args):
    # Compatible dengan paho v1 (flags, rc) dan v2 (flags, reason_code, props)
    rc = args[1] if len(args) >= 2 else args[0]
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung ke broker -> Node {NODE_ID}")
        for sig in SIGNALS:
            topic = f"{TOPIC_BASE}/node_{NODE_ID}/cs_{sig}"
            client.subscribe(topic)
            print(f"[MQTT]   sub: {topic}")
    else:
        print(f"[MQTT] Gagal rc={rc_val}")

# ─── Setup figure ─────────────────────────────────────────────────────────────
plt.ion()
fig = plt.figure(figsize=(14, 10))
fig.canvas.manager.set_window_title(
    f'CS Live Visualizer | Node {NODE_ID} | M={CS_M} ({CS_M*100//CS_N}%)')

gs_outer = gridspec.GridSpec(3, 1, figure=fig, hspace=0.5)
gs_imu   = gs_outer[0].subgridspec(1, 3, wspace=0.35)
gs_gyro  = gs_outer[1].subgridspec(1, 3, wspace=0.35)
gs_ppg   = gs_outer[2].subgridspec(1, 3, wspace=0.35)

axes = {}
for i, sig in enumerate(["ax","ay","az"]):
    axes[sig] = fig.add_subplot(gs_imu[0, i])
for i, sig in enumerate(["gx","gy","gz"]):
    axes[sig] = fig.add_subplot(gs_gyro[0, i])
axes["ir"]       = fig.add_subplot(gs_ppg[0, 0])
axes["hr_trend"] = fig.add_subplot(gs_ppg[0, 1])
axes["corr"]     = fig.add_subplot(gs_ppg[0, 2])

COLORS = {
    "ax":"#2196F3","ay":"#4CAF50","az":"#FF9800",
    "gx":"#9C27B0","gy":"#F44336","gz":"#00BCD4",
    "ir":"#E91E63",
}

# ─── Inisialisasi line objects untuk update tanpa cla() ───────────────────────
# cla() setiap frame → lambat. Lebih cepat: set_ydata() pada Line2D yang sudah ada.
total_samples = CS_N * HISTORY_WINDOWS
x_axis = np.arange(total_samples)

_lines = {}
for sig in SIGNALS:
    ax = axes[sig]
    line, = ax.plot(x_axis, history[sig], color=COLORS[sig], linewidth=1)
    _lines[sig] = line
    ax.axvline(total_samples - CS_N, color='gray',
               linestyle='--', alpha=0.4, linewidth=0.8)
    unit = UNITS[sig]
    ax.set_title(f"{sig} ({unit})", fontsize=9, pad=3)
    ax.set_ylabel(unit, fontsize=7)
    ax.tick_params(labelsize=7)
    ax.grid(True, alpha=0.25)
    ax.set_xlim(0, total_samples - 1)

# HR trend — pakai cla karena data length berubah
axes["hr_trend"].set_title("HR Trend", fontsize=9, pad=3)
axes["hr_trend"].set_ylabel("BPM", fontsize=7)
axes["hr_trend"].set_xlabel("Window ke-", fontsize=7)
axes["hr_trend"].tick_params(labelsize=7)
axes["hr_trend"].grid(True, alpha=0.25)

# Kualitas sinyal
axes["corr"].set_title("Kualitas Sinyal", fontsize=9, pad=3)
axes["corr"].set_ylabel("Score", fontsize=7)
axes["corr"].set_xlabel("Window ke-", fontsize=7)
axes["corr"].tick_params(labelsize=7)
axes["corr"].grid(True, alpha=0.25, axis='y')
axes["corr"].set_ylim(0, 1.05)

fig.canvas.draw()
plt.pause(0.01)

# ─── Process window — dipanggil dari main thread ──────────────────────────────
def process_window(win_data: dict):
    global hr_history, corr_history

    data   = win_data["data"]
    win_n  = win_data["win"]
    hr     = win_data["hr"]
    finger = win_data["finger"]
    ts     = win_data["ts"]

    # Update rolling history
    for sig in SIGNALS:
        if sig in data:
            history[sig] = np.roll(history[sig], -CS_N)
            history[sig][-CS_N:] = data[sig]

    hr_history.append(hr)
    if len(hr_history) > MAX_HIST:
        hr_history.pop(0)

    corr_proxy = []
    for sig in ["ax","ay","az","gx","gy","gz"]:
        if sig in data:
            x = data[sig]
            snr = np.abs(np.mean(x)) / (np.std(x) + 1e-9)
            corr_proxy.append(min(snr / 10.0, 1.0))
    if corr_proxy:
        corr_history.append(float(np.mean(corr_proxy)))
        if len(corr_history) > MAX_HIST:
            corr_history.pop(0)

    # ── Update sinyal — set_ydata() jauh lebih cepat dari cla()+plot() ────────
    for sig in SIGNALS:
        _lines[sig].set_ydata(history[sig])
        ax = axes[sig]
        # Auto-scale Y
        ymin, ymax = history[sig].min(), history[sig].max()
        margin = max((ymax - ymin) * 0.1, 0.01)
        ax.set_ylim(ymin - margin, ymax + margin)

    # Update title IR dengan status jari (teks ASCII, tanpa emoji)
    finger_str = "[JARI ON]" if finger else "[no finger]"
    axes["ir"].set_title(f"ir ADC | {finger_str}", fontsize=9, pad=3)

    # ── HR trend — masih pakai cla karena panjang data berubah ────────────────
    ax_hr = axes["hr_trend"]
    ax_hr.cla()
    if hr_history:
        ax_hr.plot(hr_history, 'r-o', markersize=3, linewidth=1.2)
        ax_hr.axhline(60,  color='orange', linestyle='--', alpha=0.5,
                      linewidth=0.8, label='60 BPM')
        ax_hr.axhline(100, color='red',    linestyle='--', alpha=0.5,
                      linewidth=0.8, label='100 BPM')
        ax_hr.set_ylim(0, 150)
    ax_hr.set_title(f"HR Trend | {hr} BPM", fontsize=9, pad=3)
    ax_hr.set_ylabel("BPM", fontsize=7)
    ax_hr.set_xlabel("Window ke-", fontsize=7)
    ax_hr.tick_params(labelsize=7)
    ax_hr.grid(True, alpha=0.25)
    ax_hr.legend(fontsize=6, loc='upper right')

    # ── Kualitas sinyal ────────────────────────────────────────────────────────
    ax_c = axes["corr"]
    ax_c.cla()
    if corr_history:
        c_arr = np.array(corr_history)
        bar_colors = ['green' if v > 0.7 else 'orange' if v > 0.4 else 'red'
                      for v in c_arr]
        ax_c.bar(range(len(corr_history)), corr_history,
                 color=bar_colors, width=0.8, alpha=0.8)
        ax_c.axhline(0.7, color='green', linestyle='--', alpha=0.5, linewidth=0.8)
    ax_c.set_ylim(0, 1.05)
    ax_c.set_title("Kualitas Sinyal", fontsize=9, pad=3)
    ax_c.set_ylabel("Score", fontsize=7)
    ax_c.set_xlabel("Window ke-", fontsize=7)
    ax_c.tick_params(labelsize=7)
    ax_c.grid(True, alpha=0.25, axis='y')

    fig.suptitle(
        f'CS Live | Node {NODE_ID} | Win #{win_n} | '
        f'ts={ts}ms | HR={hr} BPM | M={CS_M} ({CS_M*100//CS_N}%)',
        fontsize=10, fontweight='bold')

    # blit=False karena ada multiple axes — draw() lebih aman dari blit
    fig.canvas.draw()
    fig.canvas.flush_events()

    print(f"[Viz] Win #{win_n:4d} | HR={hr:3d} | finger={'Y' if finger else 'N'} | ts={ts}ms")

# ─── Main ─────────────────────────────────────────────────────────────────────
print("=" * 55)
print(f"  CS Live Visualizer | Node {NODE_ID}")
print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | alpha={LASSO_ALPHA}")
print(f"  {HISTORY_WINDOWS} windows rolling history ({HISTORY_WINDOWS*CS_N*10}ms)")
print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
print("=" * 55)

# Buat MQTT client — support paho v1 dan v2
if _PAHO_V2:
    mqttClient = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
else:
    mqttClient = mqtt.Client()

mqttClient.on_connect = on_connect
mqttClient.on_message = on_message

try:
    mqttClient.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
except Exception as e:
    print(f"[ERROR] Tidak bisa konek ke broker {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  -> {e}")
    exit(1)

mqttClient.loop_start()
print("\nMenunggu data... (Ctrl+C untuk keluar)\n")

try:
    while True:
        if window_queue:
            # Proses semua window yang menumpuk dalam 1 iterasi
            # Tapi hanya render plot untuk yang TERAKHIR (skip intermediate)
            # → tidak lag walau data datang lebih cepat dari render
            while len(window_queue) > 1:
                skipped = window_queue.popleft()
                # Tetap update history meski plot-nya di-skip
                for sig in SIGNALS:
                    if sig in skipped["data"]:
                        history[sig] = np.roll(history[sig], -CS_N)
                        history[sig][-CS_N:] = skipped["data"][sig]
                hr_history.append(skipped["hr"])
                if len(hr_history) > MAX_HIST:
                    hr_history.pop(0)
                print(f"[Viz] Win #{skipped['win']:4d} | (skip render, queue catchup)")

            # Render window terakhir
            process_window(window_queue.popleft())
        else:
            plt.pause(0.05)  # hanya pause saat queue kosong
except KeyboardInterrupt:
    print("\n[INFO] Berhenti.")
finally:
    mqttClient.loop_stop()
    mqttClient.disconnect()
    plt.close('all')