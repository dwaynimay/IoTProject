"""
listener.py — MQTT subscribe + dispatch ke NodeState.

Tanggung jawab tunggal:
    Terhubung ke MQTT broker, subscribe topic cs_imu dan cs_ppg,
    parse JSON payload, lalu dispatch ke NodeState yang sesuai.

Tidak ada logika rekonstruksi. Tidak ada state per-node di sini.
"""

import json
import logging

import paho.mqtt.client as mqtt

try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False

from core import StorageManager
from .node_state import NodeState

logger = logging.getLogger(__name__)


def run(
    nodes:      "dict[int, NodeState]",
    *,
    broker:     str,
    port:       int,
    keepalive:  int,
    topic_base: str,
    storage:    "StorageManager",
    processor_fn,
    validator,
    assessor,
) -> None:
    """
    Jalankan MQTT listener (blocking — panggil dari thread atau main).

    Args:
        nodes       : dict node_id → NodeState (diisi otomatis saat node baru)
        broker      : hostname/IP broker MQTT
        port        : port broker
        keepalive   : interval keepalive dalam detik
        topic_base  : prefix topic (contoh: "health_monitor")
        storage     : instance StorageManager untuk log node_registered
        processor_fn: fungsi process_window() dari processor.py
        validator   : instance ValidatorRegistry
        assessor    : instance QualityAssessor
    """

    def _get_or_create_node(node_id: int) -> NodeState:
        if node_id not in nodes:
            nodes[node_id] = NodeState(
                node_id      = node_id,
                processor_fn = processor_fn,
                validator    = validator,
                assessor     = assessor,
                storage      = storage,
            )
            logger.info("Node %d terdaftar", node_id)
            storage.log_event(node_id, "NODE_REGISTERED", f"node_id={node_id}")
        return nodes[node_id]

    def _on_connect(client, userdata, flags, rc, properties=None) -> None:
        rc_val = rc if isinstance(rc, int) else rc.value
        if rc_val == 0:
            logger.info("MQTT terhubung ke %s:%d", broker, port)
            for topic_type in ("cs_imu", "cs_ppg"):
                topic = f"{topic_base}/+/{topic_type}"
                client.subscribe(topic)
                logger.info("Subscribe: %s", topic)
        else:
            logger.error("MQTT gagal konek rc=%d", rc_val)

    def _on_message(client, userdata, message) -> None:
        try:
            payload = json.loads(message.payload.decode())
        except Exception as exc:
            logger.error("JSON parse error: %s", exc)
            return

        parts = message.topic.split("/")
        if len(parts) < 3:
            return

        try:
            node_id = int(parts[1].split("_")[1])
        except (IndexError, ValueError):
            logger.warning("Topic format tidak dikenal: %s", message.topic)
            return

        sig_type = parts[2]
        node     = _get_or_create_node(node_id)

        if sig_type == "cs_imu":
            node.on_imu(payload)
        elif sig_type == "cs_ppg":
            node.on_ppg(payload)

    # ── Build client ──────────────────────────────────────────────────────────
    if _PAHO_V2:
        client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    else:
        client = mqtt.Client()

    client.on_connect = _on_connect
    client.on_message = _on_message

    client.connect(broker, port, keepalive=keepalive)
    client.loop_forever()
