# File: server/apps/dashboard/websocket.py

import asyncio
import time
from typing import Optional
from fastapi import WebSocket, WebSocketDisconnect

from apps.dashboard.hub import hub, storage


def _now_ms() -> int:
    return int(time.time() * 1000)


def _ms_ago(ts_ms: int) -> Optional[float]:
    if not ts_ms:
        return None
    return round((_now_ms() - ts_ms) / 1000, 1)


async def ws_stream(websocket: WebSocket):
    """
    Stream real-time setiap window selesai rekonstruksi.
    """
    await hub.connect_stream(websocket)
    try:
        # Kirim snapshot status semua node saat client baru connect
        node_ids = storage.get_all_node_ids()
        snapshot_nodes = []
        for nid in node_ids:
            s = storage.get_node_stats(nid)
            s["last_seen_ago_s"] = _ms_ago(s["last_seen_ms"])
            snapshot_nodes.append(s)

        await websocket.send_json({
            "type":    "snapshot",
            "nodes":   snapshot_nodes,
            "ts_ms":   _now_ms(),
        })

        # Keep-alive loop — tunggu pesan dari client (disconnect) atau kirim ping
        while True:
            try:
                # Tunggu pesan dari client (termasuk close frame) dengan timeout 30s
                await asyncio.wait_for(websocket.receive_text(), timeout=30)
            except asyncio.TimeoutError:
                # Timeout → kirim ping, lanjut loop
                await websocket.send_json({"type": "ping", "ts_ms": _now_ms()})
            # WebSocketDisconnect akan di-raise oleh receive_text() saat client disconnect

    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        await hub.disconnect(websocket)


async def ws_events(websocket: WebSocket):
    """
    Stream event anomali real-time (LOW_QUALITY, CRITICAL, VALIDATION_ERROR, dll).
    """
    await hub.connect_events(websocket)
    try:
        # Kirim 10 event terakhir sebagai context awal
        recent = storage.get_last_events(n=10)
        await websocket.send_json({
            "type":   "snapshot",
            "events": recent,
            "ts_ms":  _now_ms(),
        })

        while True:
            try:
                await asyncio.wait_for(websocket.receive_text(), timeout=30)
            except asyncio.TimeoutError:
                await websocket.send_json({"type": "ping", "ts_ms": _now_ms()})

    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        await hub.disconnect(websocket)
