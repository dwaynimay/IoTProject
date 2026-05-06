"""
cs_reconstruct_server_deploy.py — Server rekonstruksi CS untuk sistem nyata

Subscribe ke 7 topic per node:
  health_monitor/node_N/cs_ax  → rekonstruksi ax[64]
  ...
  health_monitor/node_N/cs_ir  → rekonstruksi ir[64]

CHANGELOG v2:
  - Fix RuntimeWarning overflow LCG dengan np.errstate
  - Update paho-mqtt ke CallbackAPIVersion v2 (hilangkan DeprecationWarning)
  - Toleransi timestamp spread dinaikkan: 200ms → 800ms
    (7 paket × 1ms jeda = ~7ms actual, tapi network jitter bisa up to 100ms)
    Setelah fix firmware (timestamp konsisten), spread harusnya < 50ms.
  - Tambah per-node window counter & throughput log

Instalasi: pip install paho-mqtt numpy scikit-learn scipy
"""

import json
import time
import threading
import warnings
import numpy as np
from scipy.fftpack import idct
from sklearn.linear_model import Lasso
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

# ─── Parameter CS — HARUS sama dengan CS_Sensor.h ────────────────────────────
CS_N        = 64
CS_M        = 32
CS_PHI_SEED = 42

LASSO_ALPHA = 0.001   # optimal untuk M=32

MQTT_BROKER  = "192.168.1.18"
MQTT_PORT    = 1883
TOPIC_BASE   = "health_monitor"
SIGNALS      = ["ax", "ay", "az", "gx", "gy", "gz", "ir"]

# Toleransi timestamp spread antar 7 paket dari 1 window.
# Setelah fix firmware: semua 7 paket pakai ts_now yang sama → spread ≈ 0.
# Sisa toleransi 300ms untuk network jitter saja.
TS_SPREAD_TOLERANCE_MS = 300

# ─── Generate Phi ─────────────────────────────────────────────────────────────
def generate_phi(seed, m, n):
    """LCG + Box-Muller — HARUS identik dengan implementasi di ESP32."""
    # Suppress overflow warning: ini by design (uint32 wraps = intended behavior)
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

print(f"Membangkitkan Φ (M={CS_M}, N={CS_N}, seed={CS_PHI_SEED})...", end=" ")
PHI   = generate_phi(CS_PHI_SEED, CS_M, CS_N)
Psi   = idct(np.eye(CS_N), norm='ortho', axis=0)
Theta = PHI @ Psi
print("OK")

# ─── Rekonstruksi ────────────────────────────────────────────────────────────
def reconstruct(y: list) -> np.ndarray:
    """Rekonstruksi sinyal x̂ dari measurement y dengan LASSO."""
    y_arr = np.array(y, dtype=np.float64)
    lasso = Lasso(alpha=LASSO_ALPHA, max_iter=5000,
                  fit_intercept=False, tol=1e-5)
    lasso.fit(Theta, y_arr)
    return Psi @ lasso.coef_

# ─── Buffer per node — kumpulkan 7 sinyal per window ─────────────────────────
class NodeState:
    def __init__(self, node_id: int):
        self.node_id      = node_id
        self._buffer      = {}  # signal_name → payload dict
        self._lock        = threading.Lock()
        self.windows_done = 0
        self._last_window_time = 0.0
        self._total_rekon_ms   = 0.0

    def on_signal(self, signal: str, payload: dict):
        """Dipanggil dari MQTT thread — thread safe."""
        with self._lock:
            self._buffer[signal] = payload
            if all(s in self._buffer for s in SIGNALS):
                self._reconstruct_and_reset()

    def _reconstruct_and_reset(self):
        buf = dict(self._buffer)
        self._buffer.clear()

        # Cek toleransi timestamp — setelah fix firmware harusnya < 50ms
        timestamps = [buf[s].get("ts", 0) for s in SIGNALS if "ts" in buf.get(s, {})]
        if timestamps:
            ts_spread = max(timestamps) - min(timestamps)
            if ts_spread > TS_SPREAD_TOLERANCE_MS:
                print(f"[Node {self.node_id}] WARN: timestamp spread {ts_spread}ms "
                      f"(toleransi {TS_SPREAD_TOLERANCE_MS}ms) — paket dari window berbeda?")

        # Rekonstruksi semua sinyal
        results = {}
        t_start = time.time()
        for sig in SIGNALS:
            y = buf[sig].get("y", [])
            if len(y) == CS_M:
                results[sig] = reconstruct(y)
            else:
                print(f"[Node {self.node_id}] WARN: {sig} len={len(y)}, expected {CS_M}")

        elapsed_ms = (time.time() - t_start) * 1000
        self.windows_done += 1
        self._total_rekon_ms += elapsed_ms

        # Throughput: berapa window per detik?
        now = time.time()
        if self._last_window_time > 0:
            gap_ms = (now - self._last_window_time) * 1000
        else:
            gap_ms = 0.0
        self._last_window_time = now

        # Metadata dari PPG
        hr     = buf["ir"].get("hr", -1)
        valid  = buf["ir"].get("ppg_valid", False)
        finger = buf["ir"].get("finger", False)
        ts     = buf["ax"].get("ts", 0)

        print(f"\n[Node {self.node_id}] Window #{self.windows_done} "
              f"| ts={ts}ms | gap={gap_ms:.0f}ms | HR={hr} | finger={finger} "
              f"| rekon={elapsed_ms:.1f}ms | avg={self._total_rekon_ms/self.windows_done:.1f}ms")

        for sig in ["ax","ay","az","gx","gy","gz"]:
            if sig in results:
                x = results[sig]
                unit = "m/s²" if sig.startswith("a") else "°/s"
                print(f"  {sig}: [{x.min():.3f} … {x.max():.3f}] {unit}")

        if "ir" in results:
            x = results["ir"]
            print(f"  ir: [{x.min():.0f} … {x.max():.0f}] ADC")

        # ── TODO: simpan ke database / analisis ──────────────────────────────
        # Contoh penggunaan hasil rekonstruksi:
        #
        # 1. Simpan ke InfluxDB:
        #    write_influx(node_id, ts, results, hr, finger)
        #
        # 2. Kirim ke ML model:
        #    features = np.concatenate([results[s] for s in SIGNALS])
        #    prediction = model.predict(features.reshape(1,-1))
        #
        # 3. Publish rekonstruksi ke topic MQTT baru:
        #    payload = {"ts": ts, "ax": results["ax"].tolist(), ...}
        #    mqtt_pub.publish(f"health_monitor/node_{node_id}/reconstructed",
        #                     json.dumps(payload))

# ─── MQTT ─────────────────────────────────────────────────────────────────────
nodes: dict = {}

def get_node(node_id: int) -> NodeState:
    if node_id not in nodes:
        nodes[node_id] = NodeState(node_id)
        print(f"[INFO] Node {node_id} terdaftar")
    return nodes[node_id]

def on_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode())
    except Exception as e:
        print(f"[ERROR] JSON: {e}")
        return

    # health_monitor/node_1/cs_ax → node_id=1, signal="ax"
    parts = message.topic.split("/")
    if len(parts) < 3:
        return
    try:
        node_id = int(parts[1].split("_")[1])
    except (IndexError, ValueError):
        return

    signal = parts[2].replace("cs_", "")  # "cs_ax" → "ax"
    if signal not in SIGNALS:
        return

    get_node(node_id).on_signal(signal, payload)

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[MQTT] Terhubung ke {MQTT_BROKER}:{MQTT_PORT}")
        # Subscribe semua topic CS dari semua node
        for sig in SIGNALS:
            topic = f"{TOPIC_BASE}/+/cs_{sig}"
            client.subscribe(topic)
            print(f"[MQTT] Subscribe: {topic}")
    else:
        print(f"[MQTT] Gagal rc={rc}")

if __name__ == "__main__":
    print("=" * 55)
    print("  CS Reconstruction Server — Sistem Nyata v2")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | LASSO α={LASSO_ALPHA}")
    print(f"  Seed={CS_PHI_SEED} | Window={CS_N*10}ms")
    print(f"  7 sinyal per window: {', '.join(SIGNALS)}")
    print(f"  TS spread tolerance: {TS_SPREAD_TOLERANCE_MS}ms")
    print("=" * 55)

    # Paho-mqtt v2: gunakan CallbackAPIVersion untuk hilangkan DeprecationWarning
    try:
        client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    except Exception:
        # Fallback untuk paho-mqtt v1 lama
        client = mqtt.Client()

    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"\n[ERROR] Tidak bisa konek ke broker {MQTT_BROKER}:{MQTT_PORT}")
        print(f"  → {e}")
        print(f"\nCek:")
        print(f"  1. PC sudah konek ke WiFi yang sama dengan broker")
        print(f"  2. Broker Mosquitto sudah jalan: 'mosquitto -v'")
        print(f"  3. Firewall tidak blokir port 1883")
        exit(1)

    print("\nMenunggu data dari sensor node...\n")
    client.loop_forever()