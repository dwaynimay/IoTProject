"""
server/apps/reconstruct_server.py
Pengganti cs_reconstruct_server.py

Subscribe ke 7 topic CS per node, rekonstruksi sinyal tiap window penuh.

Jalankan dari root project:
    python -m apps.reconstruct_server
"""

import json
import time
import threading
import warnings

import paho.mqtt.client as mqtt
try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

from core.config import (
    CS_N, CS_M, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE,
    TOPIC_BASE, SIGNALS, UNITS, TS_SPREAD_TOLERANCE_MS,
)
from core.cs_router import reconstruct


# =============================================================================
# NodeState — buffer per node, thread-safe
# =============================================================================
class NodeState:
    def __init__(self, node_id: int):
        self.node_id       = node_id
        self._buffer       = {}
        self._lock         = threading.Lock()
        self.windows_done  = 0
        self._last_win_t   = 0.0
        self._total_rec_ms = 0.0

    def on_signal(self, signal: str, payload: dict):
        with self._lock:
            self._buffer[signal] = payload
            if all(s in self._buffer for s in SIGNALS):
                self._process()

    def _process(self):
        buf = dict(self._buffer)
        self._buffer.clear()

        # Cek timestamp spread
        timestamps = [buf[s]["ts"] for s in SIGNALS if "ts" in buf.get(s, {})]
        if timestamps:
            spread = max(timestamps) - min(timestamps)
            if spread > TS_SPREAD_TOLERANCE_MS:
                print(f"[Node {self.node_id}] WARN: timestamp spread {spread}ms "
                      f"(toleransi {TS_SPREAD_TOLERANCE_MS}ms)")

        # Rekonstruksi semua sinyal
        results = {}
        t0 = time.time()
        for sig in SIGNALS:
            y = buf[sig].get("y", [])
            if len(y) == CS_M:
                results[sig] = reconstruct(y)
            else:
                print(f"[Node {self.node_id}] WARN: {sig} len={len(y)}, expected {CS_M}")
        elapsed_ms = (time.time() - t0) * 1000

        self.windows_done  += 1
        self._total_rec_ms += elapsed_ms
        now = time.time()
        gap_ms = (now - self._last_win_t) * 1000 if self._last_win_t else 0
        self._last_win_t = now

        hr     = buf["ir"].get("hr", -1)
        finger = buf["ir"].get("finger", False)
        ts     = buf["ax"].get("ts", 0)

        print(f"\n[Node {self.node_id}] Window #{self.windows_done} "
              f"| ts={ts}ms | gap={gap_ms:.0f}ms | HR={hr} | finger={finger} "
              f"| rekon={elapsed_ms:.1f}ms | avg={self._total_rec_ms/self.windows_done:.1f}ms")

        for sig in ["ax", "ay", "az", "gx", "gy", "gz"]:
            if sig in results:
                x    = results[sig]
                unit = UNITS[sig]
                print(f"  {sig}: [{x.min():.3f} … {x.max():.3f}] {unit}")

        if "ir" in results:
            x = results["ir"]
            print(f"  ir: [{x.min():.0f} … {x.max():.0f}] ADC")

        # ── TODO: simpan ke database / kirim ke ML model ─────────────────────
        # Contoh:
        #   write_influx(node_id, ts, results, hr, finger)
        #   prediction = model.predict(np.concatenate([results[s] for s in SIGNALS]))


# =============================================================================
# MQTT
# =============================================================================
_nodes: dict = {}

def _get_node(node_id: int) -> NodeState:
    if node_id not in _nodes:
        _nodes[node_id] = NodeState(node_id)
        print(f"[INFO] Node {node_id} terdaftar")
    return _nodes[node_id]

def _on_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode())
    except Exception as e:
        print(f"[ERROR] JSON parse: {e}")
        return

    parts = message.topic.split("/")
    if len(parts) < 3:
        return
    try:
        node_id = int(parts[1].split("_")[1])
    except (IndexError, ValueError):
        return

    signal = parts[2].replace("cs_", "")
    if signal not in SIGNALS:
        return

    _get_node(node_id).on_signal(signal, payload)

def _on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung ke {MQTT_BROKER}:{MQTT_PORT}")
        for sig in SIGNALS:
            topic = f"{TOPIC_BASE}/+/cs_{sig}"
            client.subscribe(topic)
            print(f"[MQTT] Subscribe: {topic}")
    else:
        print(f"[MQTT] Gagal rc={rc_val}")


# =============================================================================
# Main
# =============================================================================
if __name__ == "__main__":
    warnings.filterwarnings("ignore", category=RuntimeWarning)

    print("=" * 55)
    print("  CS Reconstruction Server")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%)")
    print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  Sinyal: {', '.join(SIGNALS)}")
    print(f"  TS spread tolerance: {TS_SPREAD_TOLERANCE_MS}ms")
    print("=" * 55)

    if _PAHO_V2:
        client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    else:
        client = mqtt.Client()

    client.on_connect = _on_connect
    client.on_message = _on_message

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=MQTT_KEEPALIVE)
    except Exception as e:
        print(f"\n[ERROR] Tidak bisa konek ke {MQTT_BROKER}:{MQTT_PORT}")
        print(f"  → {e}")
        print("\nCek: Mosquitto running? ('mosquitto -v')")
        exit(1)

    print("\nMenunggu data dari sensor node...\n")
    client.loop_forever()