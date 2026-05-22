"""
server/tools/live_visualizer.py

Tool debugging: Visualisasi real-time hasil rekonstruksi CS via matplotlib.
Jalankan standalone — subscribe sendiri ke MQTT.

⚠️  Ini BUKAN bagian server production. Ini tool development/debug.

Jalankan dari server/:
    python -m tools.live_visualizer
"""

import json
import warnings
import collections
import threading
import numpy as np

warnings.filterwarnings("ignore", category=UserWarning)
warnings.filterwarnings("ignore", category=RuntimeWarning)

import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

import paho.mqtt.client as mqtt
try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

from core.config import (
    CS_N, CS_M, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE,
    TOPIC_BASE, SIGNALS, IMU_SIGNALS, UNITS, COLORS,
    HISTORY_WINDOWS, MAX_HIST, TOTAL_SAMPLES,
)
from core.cs_router import reconstruct

NODE_ID = 1  # ubah sesuai node yang ingin divisualisasi


# =============================================================================
# Buffer MQTT — thread-safe
# =============================================================================
_buf_lock    = threading.Lock()
_imu_buf     = {}   # buffer cs_imu per node_id
_ppg_buf     = {}   # buffer cs_ppg per node_id

# maxlen=10: buffer window yang menumpuk saat plot lag
window_queue = collections.deque(maxlen=10)

history      = {s: np.zeros(TOTAL_SAMPLES) for s in SIGNALS}
hr_history   = []
corr_history = []
meta         = {"win": 0}


def _on_message(client, userdata, msg):
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

    sig_type = parts[2]

    with _buf_lock:
        if sig_type == "cs_imu":
            _imu_buf[node_id] = payload
        elif sig_type == "cs_ppg":
            _ppg_buf[node_id] = payload

        # Proses kalau keduanya sudah ada
        if node_id in _imu_buf and node_id in _ppg_buf:
            imu_data = _imu_buf.pop(node_id)
            ppg_data = _ppg_buf.pop(node_id)

            results = {}
            for sig in IMU_SIGNALS:
                y = imu_data.get(sig, [])
                if len(y) == CS_M:
                    results[sig] = reconstruct(y)

            y_ir = ppg_data.get("ir", [])
            if len(y_ir) == CS_M:
                results["ir"] = reconstruct(y_ir)

            meta["win"] += 1
            window_queue.append({
                "win"   : meta["win"],
                "hr"    : ppg_data.get("hr", 0),
                "finger": ppg_data.get("finger", False),
                "ts"    : imu_data.get("ts", 0),
                "data"  : results,
            })


def _on_connect(client, userdata, *args):
    rc = args[1] if len(args) >= 2 else args[0]
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung → Node {NODE_ID}")
        for topic_type in ["cs_imu", "cs_ppg"]:
            topic = f"{TOPIC_BASE}/node_{NODE_ID}/{topic_type}"
            client.subscribe(topic)
            print(f"[MQTT] sub: {topic}")
    else:
        print(f"[MQTT] Gagal rc={rc_val}")


# =============================================================================
# Setup figure
# =============================================================================
plt.ion()
fig = plt.figure(figsize=(14, 10))
fig.canvas.manager.set_window_title(
    f'CS Live Visualizer | Node {NODE_ID} | M={CS_M} ({CS_M*100//CS_N}%)')

gs_outer = gridspec.GridSpec(3, 1, figure=fig, hspace=0.5)
gs_imu   = gs_outer[0].subgridspec(1, 3, wspace=0.35)
gs_gyro  = gs_outer[1].subgridspec(1, 3, wspace=0.35)
gs_ppg   = gs_outer[2].subgridspec(1, 3, wspace=0.35)

axes = {}
for i, sig in enumerate(["ax", "ay", "az"]):
    axes[sig] = fig.add_subplot(gs_imu[0, i])
for i, sig in enumerate(["gx", "gy", "gz"]):
    axes[sig] = fig.add_subplot(gs_gyro[0, i])
axes["ir"]       = fig.add_subplot(gs_ppg[0, 0])
axes["hr_trend"] = fig.add_subplot(gs_ppg[0, 1])
axes["corr"]     = fig.add_subplot(gs_ppg[0, 2])

# Inisialisasi line objects — update via set_ydata(), lebih cepat dari cla()
x_axis = np.arange(TOTAL_SAMPLES)
_lines = {}
for sig in SIGNALS:
    ax = axes[sig]
    line, = ax.plot(x_axis, history[sig], color=COLORS[sig], linewidth=1)
    _lines[sig] = line
    ax.axvline(TOTAL_SAMPLES - CS_N, color='gray',
               linestyle='--', alpha=0.4, linewidth=0.8)
    ax.set_title(f"{sig} ({UNITS[sig]})", fontsize=9, pad=3)
    ax.set_ylabel(UNITS[sig], fontsize=7)
    ax.tick_params(labelsize=7)
    ax.grid(True, alpha=0.25)
    ax.set_xlim(0, TOTAL_SAMPLES - 1)

axes["hr_trend"].set_title("HR Trend", fontsize=9, pad=3)
axes["hr_trend"].set_ylabel("BPM", fontsize=7)
axes["hr_trend"].tick_params(labelsize=7)
axes["hr_trend"].grid(True, alpha=0.25)

axes["corr"].set_title("Kualitas Sinyal", fontsize=9, pad=3)
axes["corr"].set_ylabel("Score", fontsize=7)
axes["corr"].tick_params(labelsize=7)
axes["corr"].grid(True, alpha=0.25, axis='y')
axes["corr"].set_ylim(0, 1.05)

fig.canvas.draw()
plt.pause(0.01)


# =============================================================================
# Process window — dipanggil dari main thread
# =============================================================================
def _process_window(win_data: dict):
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
    for sig in ["ax", "ay", "az", "gx", "gy", "gz"]:
        if sig in data:
            x   = data[sig]
            snr = np.abs(np.mean(x)) / (np.std(x) + 1e-9)
            corr_proxy.append(min(snr / 10.0, 1.0))
    if corr_proxy:
        corr_history.append(float(np.mean(corr_proxy)))
        if len(corr_history) > MAX_HIST:
            corr_history.pop(0)

    # Update sinyal — set_ydata() jauh lebih cepat dari cla()+plot()
    for sig in SIGNALS:
        _lines[sig].set_ydata(history[sig])
        ax = axes[sig]
        ymin, ymax = history[sig].min(), history[sig].max()
        margin = max((ymax - ymin) * 0.1, 0.01)
        ax.set_ylim(ymin - margin, ymax + margin)

    finger_str = "[JARI ON]" if finger else "[no finger]"
    axes["ir"].set_title(f"ir ADC | {finger_str}", fontsize=9, pad=3)

    # HR trend
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

    # Kualitas sinyal
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

    fig.canvas.draw()
    fig.canvas.flush_events()

    print(f"[Viz] Win #{win_n:4d} | HR={hr:3d} | finger={'Y' if finger else 'N'} | ts={ts}ms")


# =============================================================================
# Main
# =============================================================================
if __name__ == "__main__":
    print("=" * 55)
    print(f"  CS Live Visualizer | Node {NODE_ID}")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%)")
    print(f"  {HISTORY_WINDOWS} windows rolling ({HISTORY_WINDOWS * CS_N * 10}ms)")
    print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
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
        print(f"[ERROR] Tidak bisa konek ke {MQTT_BROKER}:{MQTT_PORT}: {e}")
        exit(1)

    mqttClient.loop_start()
    print("\nMenunggu data... (Ctrl+C untuk keluar)\n")

    try:
        while True:
            if window_queue:
                # Skip window yang menumpuk, update history tetap
                while len(window_queue) > 1:
                    skipped = window_queue.popleft()
                    for sig in SIGNALS:
                        if sig in skipped["data"]:
                            history[sig] = np.roll(history[sig], -CS_N)
                            history[sig][-CS_N:] = skipped["data"][sig]
                    hr_history.append(skipped["hr"])
                    if len(hr_history) > MAX_HIST:
                        hr_history.pop(0)
                    print(f"[Viz] Win #{skipped['win']:4d} | (skip render, queue catchup)")

                _process_window(window_queue.popleft())
            else:
                plt.pause(0.05)

    except KeyboardInterrupt:
        print("\n[INFO] Berhenti.")
    finally:
        mqttClient.loop_stop()
        mqttClient.disconnect()
        plt.close('all')