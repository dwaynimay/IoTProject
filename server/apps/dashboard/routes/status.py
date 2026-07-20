# File: server/apps/dashboard/routes/status.py

import time
from typing import Optional
from fastapi import APIRouter

from apps.dashboard.hub import hub, storage, server_stats

router = APIRouter(tags=["Overview"])


def _now_ms() -> int:
    return int(time.time() * 1000)


def _ms_ago(ts_ms: int) -> Optional[float]:
    if not ts_ms:
        return None
    return round((_now_ms() - ts_ms) / 1000, 1)


@router.get(
    "/api/status",
    summary="Status semua node",
    response_description="Daftar semua node dengan statistik ringkas",
)
def get_status():
    """
    Kembalikan status semua node yang pernah kirim data beserta
    statistik rekonstruksi dan kondisi WebSocket client saat ini.
    """
    node_ids = storage.get_all_node_ids()
    now_ms   = _now_ms()

    nodes = []
    for nid in node_ids:
        stats = storage.get_node_stats(nid)
        stats["last_seen_ago_s"] = _ms_ago(stats["last_seen_ms"])
        nodes.append(stats)

    return {
        "nodes":             nodes,
        "ws_stream_clients": hub.stream_count,
        "ws_event_clients":  hub.event_count,
        "server_uptime_s":   round((now_ms - server_stats["start_time_ms"]) / 1000, 1),
    }
