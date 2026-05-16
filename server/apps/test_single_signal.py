"""
server/apps/test_single_signal.py

Test rekonstruksi 1 sinyal dengan metrik akurasi yang bermakna.
Berguna untuk tuning OMP_K dan verifikasi Φ identik dengan firmware.

PERUBAHAN v2 (sesuai refactor gateway):
  - Firmware sekarang publish HYBRID TOPIC:
      cs_imu → semua 6 sinyal IMU dalam 1 JSON (ax,ay,az,gx,gy,gz)
      cs_ppg → sinyal IR + metadata HR + SpO2
  - Tidak ada lagi topic per-axis (cs_gx, cs_ax, dll.)
  - Field payload berubah: tidak ada "y", yang ada adalah nama sinyal langsung
      cs_imu: {"ts":..., "finger":..., "ax":[...], ..., "gz":[...]}
      cs_ppg: {"ts":..., "hr":..., "spo2":..., "ppg_valid":...,
               "finger":..., "ir":[...]}

Metrik yang ditampilkan (tanpa ground truth x_asli):
  - Measurement residual RMSE : ||y - Φ·x̂||₂ / ||y||₂
      → Seberapa baik x̂ "menjelaskan" measurement y yang diterima.
        Nilainya 0.0 = sempurna, > 0.3 = rekonstruksi buruk.
  - Sparsity koefisien OMP    : K aktif / N total
      → Berapa komponen frekuensi yang dipakai OMP.
        Harusnya jauh lebih kecil dari N (sparse).

Jalankan dari root project:
    python -m apps.test_single_signal
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

from core.config import (
    CS_N, CS_M, CS_PHI_SEED, OMP_K,
    MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, TOPIC_BASE,
    IMU_SIGNALS, PPG_SIGNALS,
)
from core.cs_router import PHI, THETA, PSI, reconstruct

# ── Konfigurasi test ──────────────────────────────────────────────────────────
NODE_ID = 1
SIGNAL  = "gx"   # sinyal yang ditest: ax/ay/az/gx/gy/gz/ir
MAX_HIST = 60

# Tentukan topic dan tipe berdasarkan sinyal yang dipilih
if SIGNAL in IMU_SIGNALS:
    MQTT_TOPIC  = f"{TOPIC_BASE}/node_{NODE_ID}/cs_imu"
    TOPIC_TYPE  = "cs_imu"
elif SIGNAL in PPG_SIGNALS:
    MQTT_TOPIC  = f"{TOPIC_BASE}/node_{NODE_ID}/cs_ppg"
    TOPIC_TYPE  = "cs_ppg"
else:
    raise ValueError(
        f"SIGNAL='{SIGNAL}' tidak dikenal. "
        f"Pilihan: {IMU_SIGNALS + PPG_SIGNALS}"
    )

# ── State ─────────────────────────────────────────────────────────────────────
data_queue       = collections.deque(maxlen=5)
history_residual = []
history_sparsity = []
window_labels    = []


def _quality_color(residual: float) -> str:
    if residual < 0.10: return 'green'
    if residual < 0.25: return 'orange'
    return 'red'


# ── Plot setup ────────────────────────────────────────────────────────────────
plt.ion()
fig = plt.figure(figsize=(13, 11))
gs  = fig.add_gridspec(3, 2, hspace=0.50, wspace=0.32)

ax_y      = fig.add_subplot(gs[0, 0])
ax_sparse = fig.add_subplot(gs[0, 1])
ax_rec    = fig.add_subplot(gs[1, :])
ax_trend  = fig.add_subplot(gs[2, :])
ax_trend2 = ax_trend.twinx()

fig.canvas.manager.set_window_title(
    f'CS Test | {SIGNAL} | Node {NODE_ID} | K={OMP_K}')
fig.suptitle(
    f'CS Test — Hadamard-Gaussian + DCT + OMP | '
    f'N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | K={OMP_K}',
    fontsize=10, fontweight='bold')


# ── Helper: ambil koefisien DCT sparse dari OMP ──────────────────────────────
def _get_omp_coeffs(y_arr: np.ndarray) -> tuple[np.ndarray, list[int]]:
    from core.cs_gaussian import omp
    s_hat   = omp(y_arr, THETA, OMP_K)
    support = [i for i in range(CS_N) if abs(s_hat[i]) > 1e-8]
    return s_hat, support


def _process(payload: dict, topic_type: str):
    global history_residual, history_sparsity, window_labels

    # ── Ekstrak y dari payload sesuai topic type ──────────────────────────────
    # cs_imu  payload: {"ts":..., "finger":..., "ax":[...], ..., "gz":[...]}
    # cs_ppg  payload: {"ts":..., "hr":..., "spo2":..., "ppg_valid":...,
    #                   "finger":..., "ir":[...]}
    y_raw  = payload.get(SIGNAL, [])
    ts     = payload.get("ts", 0)
    finger = payload.get("finger", False)
    hr     = payload.get("hr", -1)     # hanya ada di cs_ppg
    spo2   = payload.get("spo2", None) # hanya ada di cs_ppg

    if len(y_raw) != CS_M:
        print(f"[WARN] '{SIGNAL}' len={len(y_raw)}, expected {CS_M} "
              f"(topic: {topic_type})")
        return

    y_arr = np.array(y_raw, dtype=np.float64)

    # ── Rekonstruksi ─────────────────────────────────────────────────────────
    try:
        x_hat          = reconstruct(y_arr)
        s_hat, support = _get_omp_coeffs(y_arr)
    except Exception as e:
        print(f"[ERROR] Rekonstruksi: {e}")
        return

    # ── Metrik akurasi ────────────────────────────────────────────────────────
    y_hat    = PHI @ x_hat
    y_norm   = np.linalg.norm(y_arr)
    residual = float(np.linalg.norm(y_arr - y_hat) / (y_norm + 1e-9))

    k_active = len(support)
    sparsity = k_active / CS_N
    std_xhat = float(np.std(x_hat))

    history_residual.append(residual)
    history_sparsity.append(sparsity)
    window_labels.append(ts)
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
    s_mag      = np.abs(s_hat)
    bar_colors = ['C1' if i in support else 'C0' for i in range(CS_N)]
    ax_sparse.cla()
    ax_sparse.bar(range(CS_N), s_mag, color=bar_colors, width=0.8)
    ax_sparse.set_title(
        f"Koef DCT OMP | {k_active} aktif (K={OMP_K}) dari N={CS_N}",
        fontsize=9)
    ax_sparse.set_ylabel("|ŝ| magnitude")
    ax_sparse.grid(True, alpha=0.3, axis='y')

    # ── Plot 3: sinyal rekonstruksi x̂ ────────────────────────────────────────
    # Tambahkan info SpO2 di title jika sinyal PPG
    extra_info = ""
    if SIGNAL in PPG_SIGNALS:
        spo2_str = f"{spo2:.1f}%" if spo2 else "---"
        extra_info = f" | HR={hr} | SpO2={spo2_str}"

    ax_rec.cla()
    ax_rec.plot(x_hat, 'b-o', markersize=3, linewidth=1.2,
                color=qcolor,
                label=f'x̂ rekonstruksi  [finger={"Y" if finger else "N"}]')
    ax_rec.set_title(
        f"Rekonstruksi {SIGNAL} | Win #{len(history_residual)} | "
        f"ts={ts}ms | std={std_xhat:.3f}{extra_info}",
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

    ax_trend.set_ylim(0, max(0.5, max(history_residual) * 1.1)
                      if history_residual else 0.5)
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
    extra  = f" | SpO2={spo2:.1f}%" if spo2 else ""
    print(f"[Win #{len(history_residual):4d}] {status} | "
          f"residual={residual:.4f} | "
          f"K_aktif={k_active:2d}/{OMP_K} | "
          f"std={std_xhat:.3f} | "
          f"finger={'Y' if finger else 'N'}{extra}")


# ── MQTT ──────────────────────────────────────────────────────────────────────
def _on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except Exception as e:
        print(f"[ERROR] JSON: {e}")
        return

    # Tentukan topic type dari topic string
    parts = msg.topic.split("/")
    if len(parts) < 3:
        return

    topic_type = parts[2]  # "cs_imu" atau "cs_ppg"
    data_queue.append((payload, topic_type))


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
    print(f"  Field : payload['{SIGNAL}'] (topic type: {TOPIC_TYPE})")
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
                payload, topic_type = data_queue.popleft()
                _process(payload, topic_type)
            plt.pause(0.05)
    except KeyboardInterrupt:
        print("\n[INFO] Berhenti.")
    finally:
        mqttClient.loop_stop()
        mqttClient.disconnect()
        plt.close('all')
