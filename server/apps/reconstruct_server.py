# File: server/apps/reconstruct_server.py

"""
reconstruct_server.py — Server rekonstruksi CS (Hybrid Topic)

Subscribe 2 topic per node:
  health_monitor/node_N/cs_imu  → rekonstruksi ax,ay,az,gx,gy,gz sekaligus
  health_monitor/node_N/cs_ppg  → rekonstruksi ir + metadata HR

Fitur aktif:
  F1 — Validasi payload (ValidatorRegistry)
  F3 — Quality assessment per window (QualityAssessor)
  F4 — Push ke dashboard WebSocket (notify_window, notify_event)
  F6 — Simpan ke SQLite (StorageManager)

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
    TOPIC_BASE, SIGNALS, IMU_SIGNALS, PPG_SIGNALS,
    TS_SPREAD_TOLERANCE_MS, DB_PATH, RETENTION_HOURS,
)
from core.cs_router import reconstruct, PHI
from core.validator import ValidatorRegistry          # F1
from core.quality   import QualityAssessor            # F3
from core.storage   import StorageManager             # F6

# F4 — import notifier dashboard (opsional, tidak crash jika belum jalan)
try:
    from apps.dashboard_server import notify_window as _notify_window
    from apps.dashboard_server import notify_event  as _notify_event
    _DASHBOARD_AVAILABLE = True
except ImportError:
    _DASHBOARD_AVAILABLE = False
    def _notify_window(*args, **kwargs): pass
    def _notify_event(*args, **kwargs):  pass


# =============================================================================
# Konfigurasi
# =============================================================================

PURGE_EVERY_WINDOWS = 100


# =============================================================================
# Instance modul-level
# =============================================================================

_validator = ValidatorRegistry()
_assessor  = QualityAssessor(phi=PHI)
_storage   = StorageManager(db_path=DB_PATH, retention_hours=RETENTION_HOURS)

_global_window_count = 0
_global_lock         = threading.Lock()


# =============================================================================
# NodeState — buffer per node, tunggu cs_imu DAN cs_ppg
# =============================================================================

class NodeState:
    def __init__(self, node_id: int):
        self.node_id       = node_id
        self._imu_buf      = None
        self._ppg_buf      = None
        self._lock         = threading.Lock()
        self.windows_done  = 0
        self._last_win_t   = 0.0
        self._total_rec_ms = 0.0

    def on_imu(self, payload: dict):
        ok, errors = _validator.validate_imu(node_id=self.node_id, payload=payload)
        if not ok:
            msg = '; '.join(errors)
            print(f"[Node {self.node_id}] VALIDATION ERROR (cs_imu): {msg}")
            _storage.log_event(self.node_id, "VALIDATION_ERROR",
                               f"cs_imu: {msg[:400]}")
            _notify_event(self.node_id, "VALIDATION_ERROR", f"cs_imu: {msg[:200]}")
            return
        with self._lock:
            self._imu_buf = payload
            self._try_reconstruct()

    def on_ppg(self, payload: dict):
        ok, errors = _validator.validate_ppg(node_id=self.node_id, payload=payload)
        if not ok:
            msg = '; '.join(errors)
            print(f"[Node {self.node_id}] VALIDATION ERROR (cs_ppg): {msg}")
            _storage.log_event(self.node_id, "VALIDATION_ERROR",
                               f"cs_ppg: {msg[:400]}")
            _notify_event(self.node_id, "VALIDATION_ERROR", f"cs_ppg: {msg[:200]}")
            return
        with self._lock:
            self._ppg_buf = payload
            self._try_reconstruct()

    def _try_reconstruct(self):
        if self._imu_buf is None or self._ppg_buf is None:
            return

        ts_imu = self._imu_buf.get("ts", 0)
        ts_ppg = self._ppg_buf.get("ts", 0)
        spread = abs(ts_imu - ts_ppg)

        if spread > TS_SPREAD_TOLERANCE_MS:
            if ts_imu < ts_ppg:
                print(f"[Node {self.node_id}] WARN: ts spread={spread}ms — reset imu buf")
                self._imu_buf = None
            else:
                print(f"[Node {self.node_id}] WARN: ts spread={spread}ms — reset ppg buf")
                self._ppg_buf = None
            return

        imu_data = self._imu_buf
        ppg_data = self._ppg_buf
        self._imu_buf = None
        self._ppg_buf = None

        self._reconstruct(imu_data, ppg_data)

    def _reconstruct(self, imu_data: dict, ppg_data: dict):
        global _global_window_count

        results      = {}
        measurements = {}
        t0 = time.time()

        for sig in IMU_SIGNALS:
            y = imu_data.get(sig, [])
            if len(y) == CS_M:
                results[sig]      = reconstruct(y)
                measurements[sig] = y
            else:
                print(f"[Node {self.node_id}] WARN: {sig} len={len(y)}, expected {CS_M}")

        y_ir = ppg_data.get("ir", [])
        if len(y_ir) == CS_M:
            results["ir"]      = reconstruct(y_ir)
            measurements["ir"] = y_ir

        elapsed_ms = (time.time() - t0) * 1000

        self.windows_done  += 1
        self._total_rec_ms += elapsed_ms
        now    = time.time()
        gap_ms = (now - self._last_win_t) * 1000 if self._last_win_t else 0
        self._last_win_t = now
        avg_ms = self._total_rec_ms / self.windows_done

        hr     = ppg_data.get("hr", -1)
        spo2   = ppg_data.get("spo2", None)
        finger = ppg_data.get("finger", False)
        ts     = imu_data.get("ts", 0)

        # ── F3: Quality assessment ────────────────────────────────────────────
        report = _assessor.assess_window(
            results      = results,
            measurements = measurements,
            node_id      = self.node_id,
            window_num   = self.windows_done,
        )

        # ── F6: Simpan ke SQLite ──────────────────────────────────────────────
        _storage.save_window(
            node_id    = self.node_id,
            window_num = self.windows_done,
            ts_sensor  = ts,
            results    = results,
            report     = report,
            hr         = hr if hr is not None else -1,
            spo2       = spo2 if spo2 is not None else 0.0,
            finger     = finger,
        )

        # ── F4: Push ke dashboard WebSocket ──────────────────────────────────
        _notify_window(
            node_id    = self.node_id,
            window_num = self.windows_done,
            ts         = ts,
            hr         = hr if hr is not None else -1,
            spo2       = spo2,
            finger     = finger,
            report     = report,
            elapsed_ms = elapsed_ms,
        )

        # Log event anomali ke storage DAN push ke dashboard
        if report.has_critical():
            sigs = " ".join(report.critical_signals())
            detail = f"win={self.windows_done} signals={sigs}"
            _storage.log_event(self.node_id, "CRITICAL", detail)
            _notify_event(self.node_id, "CRITICAL", detail)
        elif report.has_low_quality():
            sigs = " ".join(report.low_quality_signals())
            detail = f"win={self.windows_done} signals={sigs}"
            _storage.log_event(self.node_id, "LOW_QUALITY", detail)
            _notify_event(self.node_id, "LOW_QUALITY", detail)

        # Purge periodik
        with _global_lock:
            _global_window_count += 1
            do_purge = (_global_window_count % PURGE_EVERY_WINDOWS == 0)
        if do_purge:
            _storage.purge_old()

        # ── Console output ────────────────────────────────────────────────────
        spo2_str = f" | SpO2={spo2:.1f}%" if spo2 is not None else ""
        q_tag    = ""
        if report.has_critical():
            q_tag = " ⚠ CRITICAL"
        elif report.has_low_quality():
            q_tag = " ⚠ LOW_Q"

        print(f"\n[Node {self.node_id}] Window #{self.windows_done} "
              f"| ts={ts}ms | gap={gap_ms:.0f}ms "
              f"| HR={hr}{spo2_str} | finger={'Y' if finger else 'N'} "
              f"| rekon={elapsed_ms:.1f}ms | avg={avg_ms:.1f}ms"
              f"{q_tag}")
        print(f"  {report.summary()}")
        for line in report.detail_lines():
            print(line)

        if self.windows_done % 20 == 0:
            print(f"  {_assessor.stats_summary()}")
        if self.windows_done % 20 == 0:
            vstats = _validator.get_stats()
            print(f"  [Validator] {vstats}")
        if self.windows_done % 50 == 0:
            size_kb = _storage.db_size_bytes() / 1024
            print(f"  [Storage] DB size={size_kb:.1f} KB | "
                  f"node_stats={_storage.get_node_stats(self.node_id)}")


# =============================================================================
# MQTT
# =============================================================================

_nodes: dict = {}

def _get_node(node_id: int) -> NodeState:
    if node_id not in _nodes:
        _nodes[node_id] = NodeState(node_id)
        print(f"[INFO] Node {node_id} terdaftar")
        _storage.log_event(node_id, "NODE_REGISTERED", f"node_id={node_id}")
        _notify_event(node_id, "NODE_REGISTERED", f"node_id={node_id}")
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

    sig_type = parts[2]
    node     = _get_node(node_id)

    if sig_type == "cs_imu":
        node.on_imu(payload)
    elif sig_type == "cs_ppg":
        node.on_ppg(payload)

def _on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        print(f"[MQTT] Terhubung ke {MQTT_BROKER}:{MQTT_PORT}")
        for topic_type in ["cs_imu", "cs_ppg"]:
            topic = f"{TOPIC_BASE}/+/{topic_type}"
            client.subscribe(topic)
            print(f"[MQTT] Subscribe: {topic}")
    else:
        print(f"[MQTT] Gagal rc={rc_val}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    warnings.filterwarnings("ignore", category=RuntimeWarning)

    _storage.open()

    print("=" * 60)
    print("  CS Reconstruction Server (F1 + F3 + F4 + F6)")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | OMP K=20")
    print(f"  Broker : {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  DB     : {DB_PATH} (retention={RETENTION_HOURS}h)")
    print(f"  F4 Dashboard: {'aktif' if _DASHBOARD_AVAILABLE else 'tidak tersedia (jalankan dashboard_server terpisah)'}")
    print("=" * 60)

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
        _storage.close()
        exit(1)

    print("\nMenunggu data dari sensor node...\n")

    try:
        client.loop_forever()
    finally:
        _storage.close()
        print("[INFO] Storage ditutup.")
