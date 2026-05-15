"""
server/apps/reconstruct_server.py
Pengganti cs_reconstruct_server.py

Subscribe ke topic cs_imu dan cs_ppg per node, rekonstruksi sinyal tiap window penuh.

Jalankan dari root project:
    python -m apps.reconstruct_server
"""

import json
import time
import warnings

import paho.mqtt.client as mqtt
try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

from core.config import (
    CS_N, CS_M, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE,
    TOPIC_BASE, SIGNALS, UNITS
)
from core.cs_router import reconstruct

# =============================================================================
# State & Stats Tracker per node
# =============================================================================
class NodeStats:
    def __init__(self, node_id: int):
        self.node_id       = node_id
        self.windows_done  = 0
        self._last_win_t   = 0.0
        self._total_rec_ms = 0.0

    def print_stats(self, elapsed_ms: float, ts: int, hr: int, finger: bool, results: dict, sig_type: str):
        self.windows_done  += 1
        self._total_rec_ms += elapsed_ms
        now = time.time()
        gap_ms = (now - self._last_win_t) * 1000 if self._last_win_t else 0
        self._last_win_t = now

        print(f"\n[Node {self.node_id}] [{sig_type.upper()}] Window #{self.windows_done} "
              f"| ts={ts}ms | gap={gap_ms:.0f}ms | HR={hr} | finger={finger} "
              f"| rekon={elapsed_ms:.1f}ms")

        for sig in ["ax", "ay", "az", "gx", "gy", "gz"]:
            if sig in results:
                x = results[sig]
                print(f"  {sig}: [{x.min():.3f} … {x.max():.3f}] {UNITS[sig]}")

        if "ir" in results:
            x = results["ir"]
            print(f"  ir: [{x.min():.0f} … {x.max():.0f}] {UNITS['ir']}")

_nodes = {}

def _get_node(node_id: int) -> NodeStats:
    if node_id not in _nodes:
        _nodes[node_id] = NodeStats(node_id)
        print(f"[INFO] Node {node_id} terdaftar")
    return _nodes[node_id]

# =============================================================================
# MQTT Handlers
# =============================================================================
def _on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung ke {MQTT_BROKER}:{MQTT_PORT}")
        for node_id in [1, 2]:
            client.subscribe(f"{TOPIC_BASE}/node_{node_id}/cs_imu")
            client.subscribe(f"{TOPIC_BASE}/node_{node_id}/cs_ppg")
            print(f"[MQTT] Subscribe: {TOPIC_BASE}/node_{node_id}/(cs_imu | cs_ppg)")
    else:
        print(f"[MQTT] Gagal rc={rc_val}")

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

    sig_type = parts[2]
    node_stats = _get_node(node_id)
    results = {}
    
    t0 = time.time()
    
    if sig_type == "cs_imu":
        for sig in ["ax", "ay", "az", "gx", "gy", "gz"]:
            y = payload.get(sig, [])
            if len(y) == CS_M:
                results[sig] = reconstruct(y)
        
        elapsed_ms = (time.time() - t0) * 1000
        ts = payload.get("ts", 0)
        finger = payload.get("finger", False)
        node_stats.print_stats(elapsed_ms, ts, -1, finger, results, "IMU")

    elif sig_type == "cs_ppg":
        y = payload.get("y", [])
        if len(y) == CS_M:
            results["ir"] = reconstruct(y)
        
        elapsed_ms = (time.time() - t0) * 1000
        ts = payload.get("ts", 0)
        finger = payload.get("finger", False)
        hr = payload.get("hr", -1)
        node_stats.print_stats(elapsed_ms, ts, hr, finger, results, "PPG")

# =============================================================================
# Main
# =============================================================================
if __name__ == "__main__":
    warnings.filterwarnings("ignore", category=RuntimeWarning)

    print("=" * 55)
    print("  CS Reconstruction Server (Hybrid Mode)")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%)")
    print(f"  Broker: {MQTT_BROKER}:{MQTT_PORT}")
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
        exit(1)

    print("\nMenunggu data dari sensor node...\n")
    client.loop_forever()