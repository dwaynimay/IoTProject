# File: server/apps/dashboard/routes/nodes.py

import time
from typing import Optional
from fastapi import APIRouter, HTTPException

from core.config import SIGNALS
from apps.dashboard.hub import storage

router = APIRouter(tags=["Node"])


def _now_ms() -> int:
    return int(time.time() * 1000)


def _ms_ago(ts_ms: int) -> Optional[float]:
    if not ts_ms:
        return None
    return round((_now_ms() - ts_ms) / 1000, 1)


def _node_or_404(node_id: int) -> None:
    if node_id not in storage.get_all_node_ids():
        raise HTTPException(status_code=404, detail=f"Node {node_id} tidak ditemukan")


@router.get(
    "/api/nodes/{node_id}",
    summary="Detail satu node",
)
async def get_node_detail(node_id: int):
    """
    Statistik lengkap satu node, preview 1 window terakhir per sinyal,
    dan 10 event terbaru.
    """
    _node_or_404(node_id)

    stats  = storage.get_node_stats(node_id)
    stats["last_seen_ago_s"] = _ms_ago(stats["last_seen_ms"])
    events = storage.get_last_events(node_id=node_id, n=10)

    last_window: dict = {}
    for sig in SIGNALS:
        rows = storage.get_last_windows(node_id=node_id, signal=sig, n=1)
        if rows:
            r = rows[0]
            last_window[sig] = {
                "window_num":   r["window_num"],
                "rel_error":    r["rel_error"],
                "quality_flag": r["quality_flag"],
                "snr_db":       round(r["snr_db"], 2) if r["snr_db"] else None,
                "sparsity":     round(r["sparsity"], 3) if r["sparsity"] else None,
                "ts_sensor_ms": r["ts_sensor_ms"],
            }

    return {
        "stats":         stats,
        "last_window":   last_window,
        "recent_events": events,
    }
