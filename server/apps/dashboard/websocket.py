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
        def _load_snapshot() -> list[dict]:
            snapshot_nodes = []
            for nid in storage.get_all_node_ids():
                stats = storage.get_node_stats(nid)
                stats["last_seen_ago_s"] = _ms_ago(stats["last_seen_ms"])
                snapshot_nodes.append(stats)
            return snapshot_nodes

        snapshot_nodes = await asyncio.to_thread(_load_snapshot)

        if not await hub.send(websocket, {
            "type":    "snapshot",
            "nodes":   snapshot_nodes,
            "ts_ms":   _now_ms(),
        }):
            return

        # Keep-alive loop — tunggu pesan dari client (disconnect) atau kirim ping
        while True:
            try:
                # Tunggu pesan dari client (termasuk close frame) dengan timeout 30s
                await asyncio.wait_for(websocket.receive_text(), timeout=30)
            except asyncio.TimeoutError:
                # Timeout → kirim ping, lanjut loop
                if not await hub.send(websocket, {"type": "ping", "ts_ms": _now_ms()}):
                    return
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
        recent = await asyncio.to_thread(storage.get_last_events, n=10)
        if not await hub.send(websocket, {
            "type":   "snapshot",
            "events": recent,
            "ts_ms":  _now_ms(),
        }):
            return

        while True:
            try:
                await asyncio.wait_for(websocket.receive_text(), timeout=30)
            except asyncio.TimeoutError:
                if not await hub.send(websocket, {"type": "ping", "ts_ms": _now_ms()}):
                    return

    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        await hub.disconnect(websocket)
