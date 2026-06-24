"""
listener.py — MQTT subscribe + dispatch ke NodeState (group-aware).

Tanggung jawab tunggal:
    Terhubung ke MQTT broker, subscribe topic cs_imu dan cs_ppg,
    parse JSON payload, lalu dispatch ke NodeState yang sesuai
    berdasarkan NODE_GROUPS config.

Perubahan v4 (cross-node pairing):
    - Startup: build reverse lookup dari NODE_GROUPS
      (_imu_node_to_group, _ppg_node_to_group)
    - _on_message: route berdasarkan group, bukan per-node
    - Backward-compatible: jika NODE_GROUPS kosong, fallback ke per-node

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
from core.config import NODE_GROUPS
from .node_state import NodeState

logger = logging.getLogger(__name__)


def _build_group_lookups() -> tuple[dict[int, int], dict[int, int]]:
    """
    Build reverse lookup maps dari NODE_GROUPS config.

    Returns:
        imu_map : {physical_node_id → group_id} untuk cs_imu
        ppg_map : {physical_node_id → group_id} untuk cs_ppg
    """
    imu_map: dict[int, int] = {}
    ppg_map: dict[int, int] = {}

    if not NODE_GROUPS:
        return imu_map, ppg_map

    for group_id, mapping in NODE_GROUPS.items():
        imu_node = mapping.get("imu_node")
        ppg_node = mapping.get("ppg_node")
        if imu_node is not None:
            imu_map[imu_node] = group_id
        if ppg_node is not None:
            ppg_map[ppg_node] = group_id

    return imu_map, ppg_map


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
        nodes       : dict group_id → NodeState (diisi otomatis saat node baru)
        broker      : hostname/IP broker MQTT
        port        : port broker
        keepalive   : interval keepalive dalam detik
        topic_base  : prefix topic (contoh: "health_monitor")
        storage     : instance StorageManager untuk log node_registered
        processor_fn: fungsi process_window() dari processor.py
        validator   : instance ValidatorRegistry
        assessor    : instance QualityAssessor
    """
    # Build reverse lookups sekali saat startup
    _imu_to_group, _ppg_to_group = _build_group_lookups()
    _use_groups = bool(NODE_GROUPS)

    if _use_groups:
        logger.info("NODE_GROUPS aktif: %s", NODE_GROUPS)
        logger.info("  IMU routing: %s", _imu_to_group)
        logger.info("  PPG routing: %s", _ppg_to_group)
    else:
        logger.info("NODE_GROUPS kosong — mode legacy (per-node)")

    def _get_or_create_node_legacy(node_id: int) -> NodeState:
        """Legacy: satu NodeState per physical node (imu_node == ppg_node)."""
        if node_id not in nodes:
            nodes[node_id] = NodeState(
                group_id     = node_id,
                imu_node_id  = node_id,
                ppg_node_id  = node_id,
                processor_fn = processor_fn,
                validator    = validator,
                assessor     = assessor,
                storage      = storage,
            )
            logger.info("Node %d terdaftar (legacy mode)", node_id)
            storage.log_event(node_id, "NODE_REGISTERED", f"node_id={node_id}")
        return nodes[node_id]

    def _get_or_create_group(group_id: int) -> NodeState:
        """Group mode: satu NodeState per group (imu_node != ppg_node)."""
        if group_id not in nodes:
            mapping = NODE_GROUPS[group_id]
            nodes[group_id] = NodeState(
                group_id     = group_id,
                imu_node_id  = mapping["imu_node"],
                ppg_node_id  = mapping["ppg_node"],
                processor_fn = processor_fn,
                validator    = validator,
                assessor     = assessor,
                storage      = storage,
            )
            logger.info("Group %d terdaftar (imu_node=%d, ppg_node=%d)",
                        group_id, mapping["imu_node"], mapping["ppg_node"])
            storage.log_event(group_id, "NODE_REGISTERED",
                              f"group_id={group_id} imu={mapping['imu_node']} "
                              f"ppg={mapping['ppg_node']}")
        return nodes[group_id]

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

        if _use_groups:
            # ── Group mode: route berdasarkan NODE_GROUPS ─────────────────
            if sig_type == "cs_imu":
                group_id = _imu_to_group.get(node_id)
                if group_id is None:
                    logger.warning("cs_imu dari node %d — tidak ada group mapping, "
                                   "diabaikan", node_id)
                    return
                group = _get_or_create_group(group_id)
                group.on_imu(payload)

            elif sig_type == "cs_ppg":
                group_id = _ppg_to_group.get(node_id)
                if group_id is None:
                    logger.warning("cs_ppg dari node %d — tidak ada group mapping, "
                                   "diabaikan", node_id)
                    return
                group = _get_or_create_group(group_id)
                group.on_ppg(payload)
        else:
            # ── Legacy mode: per-node (backward-compatible) ───────────────
            node = _get_or_create_node_legacy(node_id)
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
