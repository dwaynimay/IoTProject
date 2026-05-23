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

@router.get(
    "/api/nodes/{node_id}/activity",
    summary="Aktivitas node (segment)",
)
async def get_node_activity(node_id: int, hours: int = 24):
    """
    Riwayat aktivitas (segment) berdasarkan kualitas data / ML labels sementara.
    """
    _node_or_404(node_id)
    
    cutoff_ms = _now_ms() - (hours * 3600 * 1000)
    
    # Akses langsung DB via storage manager internals
    # Jangan tiru ini untuk write, tapi untuk read sementara aman
    storage._ensure_open()
    rows = storage._conn.execute(
        """
        SELECT window_num, ts_server_ms, quality_flag
        FROM windows
        WHERE node_id = ? AND signal = 'ir' AND ts_server_ms > ?
        ORDER BY window_num ASC
        """,
        (node_id, cutoff_ms)
    ).fetchall()

    segments = []
    if not rows:
        return {"segments": []}

    # Collapse
    current_label = rows[0][2] or "OK"
    start_ms = rows[0][1]
    last_ms = rows[0][1]

    def _push_segment(label, start, end):
        dur_s = (end - start) / 1000.0
        # If duration is 0 (single window), give it ~2 seconds default duration
        if dur_s <= 0:
            dur_s = 2.0
            end = start + 2000
        segments.append({
            "label": label,
            "start_ms": start,
            "end_ms": end,
            "duration_s": round(dur_s, 1),
            "avg_confidence": 1.0, # proxy
        })

    for row in rows[1:]:
        wn, ts, flag = row
        label = flag or "OK"
        # Split jika beda label ATAU gap waktu > 10 detik
        if label != current_label or (ts - last_ms) > 10000:
            _push_segment(current_label, start_ms, last_ms)
            current_label = label
            start_ms = ts
        last_ms = ts

    # Push last
    _push_segment(current_label, start_ms, last_ms)

    return {"segments": segments}
