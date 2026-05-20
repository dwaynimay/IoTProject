# File: server/apps/dashboard_server.py

# =============================================================================
# dashboard_server.py — REST API + WebSocket Server (F4)
# =============================================================================
#
# Jalankan BERSAMAAN dengan reconstruct_server.py di terminal berbeda:
#
#   Terminal 1: python -m apps.reconstruct_server
#   Terminal 2: uvicorn apps.dashboard_server:app --host 0.0.0.0 --port 8000
#
# Atau langsung:
#   python -m apps.dashboard_server
#
# =============================================================================
#
# REST ENDPOINTS:
#
#   GET  /api/status
#        → status semua node (last_seen, windows, HR, SpO2, kualitas)
#
#   GET  /api/nodes/{node_id}
#        → detail satu node: stats + last window per sinyal + recent events
#
#   GET  /api/nodes/{node_id}/windows?signal=ax&n=20&include_values=false
#        → N window terakhir untuk sinyal tertentu
#
#   GET  /api/nodes/{node_id}/events?event_type=LOW_QUALITY&n=20
#        → event log untuk satu node
#
#   GET  /api/events?n=50
#        → semua event terbaru lintas node
#
#   GET  /api/metrics
#        → throughput server, avg rekon time, error rate
#
#   GET  /api/db
#        → info SQLite (ukuran, jumlah baris, retention)
#
#   POST /api/nodes/{node_id}/purge
#        → hapus data lama untuk satu node (debug/maintenance)
#
# WEBSOCKET:
#
#   WS   /ws/stream
#        → push JSON setiap window selesai rekonstruksi
#          {"type":"window","node_id":1,"window_num":5,"ts":1234,
#           "hr":72,"spo2":98.1,"finger":true,"elapsed_ms":4.2,
#           "quality":{"avg_rel_error":0.08,"any_low_quality":false,
#                      "signals":{"ax":{"rel_error":0.07,"flag":"OK",...}}}}
#
#   WS   /ws/events
#        → push JSON setiap event anomali baru
#          {"type":"event","node_id":1,"event_type":"LOW_QUALITY",
#           "detail":"win=5 signals=gx gz","ts_ms":1716123456789}
#
# CARA PAKAI DARI reconstruct_server.py:
#   from apps.dashboard_server import notify_window, notify_event
#
#   notify_window(node_id, window_num, ts, hr, spo2, finger, report, elapsed_ms)
#   notify_event(node_id, event_type, detail)
#
# =============================================================================

from __future__ import annotations

import asyncio
import time
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
import os

from core.storage import StorageManager
from core.config import (
    SIGNALS, IMU_SIGNALS, PPG_SIGNALS,
    DB_PATH, RETENTION_HOURS,
)


# =============================================================================
# BroadcastHub — distribusi pesan ke semua WebSocket client
# =============================================================================

class BroadcastHub:
    """
    Thread-safe hub untuk push pesan ke semua WebSocket client.

    reconstruct_server.py memanggil publish_window() / publish_event()
    dari thread MQTT (sync). Hub meng-schedule coroutine ke asyncio
    event loop FastAPI via run_coroutine_threadsafe().
    """

    def __init__(self) -> None:
        self._stream_clients: list[WebSocket] = []
        self._event_clients:  list[WebSocket] = []
        self._lock            = asyncio.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        """Simpan referensi event loop — dipanggil saat lifespan startup."""
        self._loop = loop

    # ── WebSocket lifecycle ───────────────────────────────────────────────────

    async def connect_stream(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._stream_clients.append(ws)

    async def connect_events(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._event_clients.append(ws)

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self._stream_clients = [c for c in self._stream_clients if c is not ws]
            self._event_clients  = [c for c in self._event_clients  if c is not ws]

    # ── Broadcast ─────────────────────────────────────────────────────────────

    async def _broadcast(self, clients: list[WebSocket], data: dict) -> None:
        """Kirim data ke semua client, hapus yang sudah disconnect."""
        dead: list[WebSocket] = []
        for ws in list(clients):
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        if dead:
            async with self._lock:
                self._stream_clients = [c for c in self._stream_clients if c not in dead]
                self._event_clients  = [c for c in self._event_clients  if c not in dead]

    async def publish_window(self, data: dict) -> None:
        await self._broadcast(self._stream_clients, data)

    async def publish_event(self, data: dict) -> None:
        await self._broadcast(self._event_clients, data)

    # ── Thread-safe publish (dipanggil dari thread non-async) ─────────────────

    def publish_window_threadsafe(self, data: dict) -> None:
        """Dipanggil dari reconstruct_server (MQTT thread)."""
        if self._loop and self._loop.is_running():
            asyncio.run_coroutine_threadsafe(self.publish_window(data), self._loop)

    def publish_event_threadsafe(self, data: dict) -> None:
        """Dipanggil dari reconstruct_server (MQTT thread)."""
        if self._loop and self._loop.is_running():
            asyncio.run_coroutine_threadsafe(self.publish_event(data), self._loop)

    @property
    def stream_count(self) -> int:
        return len(self._stream_clients)

    @property
    def event_count(self) -> int:
        return len(self._event_clients)


# =============================================================================
# Module-level singletons
# =============================================================================

hub     = BroadcastHub()
storage = StorageManager(db_path=DB_PATH, retention_hours=RETENTION_HOURS)

# Statistik ringan yang diupdate oleh notify_window / notify_event
_server_stats: dict = {
    "start_time_ms":    int(time.time() * 1000),
    "total_windows":    0,
    "total_rekon_ms":   0.0,
    "total_val_errors": 0,
    "total_low_quality": 0,
    "total_critical":    0,
}


# =============================================================================
# Lifespan — buka/tutup storage + simpan event loop
# =============================================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    storage.open()
    hub.set_loop(asyncio.get_running_loop())
    print(f"[Dashboard] Server siap | DB: {DB_PATH}")
    print(f"[Dashboard] Docs: http://0.0.0.0:8000/docs")
    yield
    # Shutdown
    storage.close()
    print("[Dashboard] Storage ditutup.")


# =============================================================================
# FastAPI app
# =============================================================================

app = FastAPI(
    title       = "Health Monitor Dashboard API",
    description = (
        "REST + WebSocket API untuk ESP32 Health Monitor.\n\n"
        "**Jalankan bersamaan dengan** `reconstruct_server.py`."
    ),
    version     = "1.0.0",
    lifespan    = lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins     = ["*"],
    allow_credentials = True,
    allow_methods     = ["*"],
    allow_headers     = ["*"],
)

# Serve static dashboard
_STATIC_DIR = os.path.join(os.path.dirname(__file__), '..', 'static')
if os.path.isdir(_STATIC_DIR):
    app.mount("/static", StaticFiles(directory=_STATIC_DIR), name="static")


# =============================================================================
# Helper
# =============================================================================

def _now_ms() -> int:
    return int(time.time() * 1000)

def _ms_ago(ts_ms: int) -> Optional[float]:
    if not ts_ms:
        return None
    return round((_now_ms() - ts_ms) / 1000, 1)

def _node_or_404(node_id: int) -> None:
    """Lempar 404 jika node belum pernah kirim data."""
    if node_id not in storage.get_all_node_ids():
        raise HTTPException(status_code=404, detail=f"Node {node_id} tidak ditemukan")


# =============================================================================
# REST — Status & Overview
# =============================================================================

@app.get(
    "/api/status",
    summary="Status semua node",
    tags=["Overview"],
    response_description="Daftar semua node dengan statistik ringkas",
)
async def get_status():
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
        "server_uptime_s":   round((now_ms - _server_stats["start_time_ms"]) / 1000, 1),
    }


@app.get(
    "/api/nodes/{node_id}",
    summary="Detail satu node",
    tags=["Node"],
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


# =============================================================================
# REST — Windows
# =============================================================================

@app.get(
    "/api/nodes/{node_id}/windows",
    summary="N window terakhir untuk satu sinyal",
    tags=["Node"],
)
async def get_windows(
    node_id: int,
    signal:  str  = Query("ax",  description="Sinyal: ax/ay/az/gx/gy/gz/ir"),
    n:       int  = Query(20,    description="Jumlah window", ge=1, le=500),
    include_values: bool = Query(False, description="Sertakan array nilai rekonstruksi (64 float)"),
):
    """
    Ambil N window terakhir untuk satu sinyal dari satu node.
    Default tanpa array nilai untuk menghemat bandwidth.
    Aktifkan `include_values=true` jika ingin plot di client.
    """
    valid_signals = SIGNALS
    if signal not in valid_signals:
        raise HTTPException(
            status_code=400,
            detail=f"Signal '{signal}' tidak valid. Pilihan: {valid_signals}",
        )

    rows = storage.get_last_windows(node_id=node_id, signal=signal, n=n)

    if not include_values:
        for r in rows:
            r.pop("values", None)

    return {
        "node_id": node_id,
        "signal":  signal,
        "count":   len(rows),
        "windows": rows,
    }


# =============================================================================
# REST — Events
# =============================================================================

@app.get(
    "/api/nodes/{node_id}/events",
    summary="Event log satu node",
    tags=["Node"],
)
async def get_node_events(
    node_id:    int,
    event_type: Optional[str] = Query(
        None,
        description="Filter tipe: LOW_QUALITY / CRITICAL / VALIDATION_ERROR / NODE_REGISTERED",
    ),
    n: int = Query(20, ge=1, le=500),
):
    """Event log untuk satu node, opsional filter per tipe."""
    _node_or_404(node_id)
    events = storage.get_last_events(node_id=node_id, event_type=event_type, n=n)
    return {"node_id": node_id, "count": len(events), "events": events}


@app.get(
    "/api/events",
    summary="Semua event terbaru lintas node",
    tags=["Overview"],
)
async def get_all_events(
    event_type: Optional[str] = Query(None, description="Filter tipe event"),
    n:          int            = Query(50, ge=1, le=500),
):
    """Semua event terbaru dari semua node, opsional filter per tipe."""
    events = storage.get_last_events(event_type=event_type, n=n)
    return {"count": len(events), "events": events}


# =============================================================================
# REST — Metrics & DB Info
# =============================================================================

@app.get(
    "/api/metrics",
    summary="Metrik server keseluruhan",
    tags=["Overview"],
)
async def get_metrics():
    """
    Statistik throughput dan kualitas sejak server start.
    Diupdate real-time oleh `notify_window()` dari reconstruct_server.
    """
    total_w  = max(_server_stats["total_windows"], 1)
    avg_ms   = _server_stats["total_rekon_ms"] / total_w
    uptime_s = (_now_ms() - _server_stats["start_time_ms"]) / 1000

    return {
        "uptime_s":            round(uptime_s, 1),
        "total_windows":       _server_stats["total_windows"],
        "avg_rekon_ms":        round(avg_ms, 2),
        "total_val_errors":    _server_stats["total_val_errors"],
        "total_low_quality":   _server_stats["total_low_quality"],
        "total_critical":      _server_stats["total_critical"],
        "val_error_rate":      round(_server_stats["total_val_errors"] / total_w, 4),
        "low_quality_rate":    round(_server_stats["total_low_quality"] / total_w, 4),
        "ws_stream_clients":   hub.stream_count,
        "ws_event_clients":    hub.event_count,
    }


@app.get(
    "/api/db",
    summary="Info database SQLite",
    tags=["Overview"],
)
async def get_db_info():
    """Ukuran file DB, jumlah baris per tabel, dan konfigurasi retention."""
    size_bytes   = storage.db_size_bytes()
    row_windows  = storage._conn.execute("SELECT COUNT(*) FROM windows").fetchone()[0]
    row_events   = storage._conn.execute("SELECT COUNT(*) FROM events").fetchone()[0]

    return {
        "path":            str(storage._path.resolve()),
        "size_bytes":      size_bytes,
        "size_kb":         round(size_bytes / 1024, 1),
        "rows_windows":    row_windows,
        "rows_events":     row_events,
        "retention_hours": storage._retention_hours,
    }


# =============================================================================
# REST — Maintenance
# =============================================================================

@app.post(
    "/api/purge",
    summary="Hapus data lama (semua node)",
    tags=["Maintenance"],
)
async def purge_old(
    max_age_hours: int = Query(
        RETENTION_HOURS,
        description="Hapus data lebih tua dari N jam",
        ge=1,
    ),
):
    """
    Hapus baris windows dan events yang lebih lama dari `max_age_hours`.
    Default menggunakan nilai retention dari config.
    """
    deleted = storage.purge_old(max_age_hours=max_age_hours)
    return {"deleted_rows": deleted, "max_age_hours": max_age_hours}


@app.delete(
    "/api/nodes/{node_id}/data",
    summary="Hapus semua data satu node",
    tags=["Maintenance"],
)
async def delete_node_data(node_id: int):
    """
    Hapus semua windows dan events untuk node tertentu.
    Berguna untuk reset node saat debugging atau ganti hardware.
    """
    with storage._lock:
        dw = storage._conn.execute(
            "DELETE FROM windows WHERE node_id=?", (node_id,)
        ).rowcount
        de = storage._conn.execute(
            "DELETE FROM events WHERE node_id=?", (node_id,)
        ).rowcount
        storage._conn.commit()
    return {"node_id": node_id, "deleted_windows": dw, "deleted_events": de}


@app.get("/", include_in_schema=False)
async def serve_dashboard():
    index = os.path.join(os.path.dirname(__file__), '..', 'static', 'index.html')
    if os.path.exists(index):
        return FileResponse(index)
    return JSONResponse({"message": "Dashboard tidak ditemukan. Letakkan index.html di server/static/"})


# =============================================================================
# WebSocket — /ws/stream
# =============================================================================

@app.websocket("/ws/stream")
async def ws_stream(websocket: WebSocket):
    """
    Stream real-time setiap window selesai rekonstruksi.

    **Format pesan window:**
    ```json
    {
      "type": "window",
      "node_id": 1,
      "window_num": 42,
      "ts": 12345,
      "hr": 72,
      "spo2": 98.1,
      "finger": true,
      "elapsed_ms": 4.2,
      "quality": {
        "avg_rel_error": 0.082,
        "any_low_quality": false,
        "signals": {
          "ax": {"rel_error": 0.071, "flag": "OK", "sparsity": 0.30, "snr_db": 21.3}
        }
      }
    }
    ```

    **Format pesan ping** (setiap 30 detik agar koneksi tidak timeout):
    ```json
    {"type": "ping", "ts_ms": 1716123456789}
    ```
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

        # Keep-alive loop
        while True:
            await asyncio.sleep(30)
            await websocket.send_json({"type": "ping", "ts_ms": _now_ms()})

    except WebSocketDisconnect:
        pass
    finally:
        await hub.disconnect(websocket)


# =============================================================================
# WebSocket — /ws/events
# =============================================================================

@app.websocket("/ws/events")
async def ws_events(websocket: WebSocket):
    """
    Stream event anomali real-time (LOW_QUALITY, CRITICAL, VALIDATION_ERROR, dll).

    **Format pesan event:**
    ```json
    {
      "type": "event",
      "node_id": 1,
      "event_type": "LOW_QUALITY",
      "detail": "win=5 signals=gx gz",
      "ts_ms": 1716123456789
    }
    ```
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
            await asyncio.sleep(30)
            await websocket.send_json({"type": "ping", "ts_ms": _now_ms()})

    except WebSocketDisconnect:
        pass
    finally:
        await hub.disconnect(websocket)


# =============================================================================
# Notifier API — dipanggil dari reconstruct_server.py
# =============================================================================

def notify_window(
    node_id:    int,
    window_num: int,
    ts:         int,
    hr:         int,
    spo2:       Optional[float],
    finger:     bool,
    report,                         # WindowReport dari quality.py
    elapsed_ms: float,
) -> None:
    """
    Dipanggil dari reconstruct_server._reconstruct() setelah window selesai.
    Push payload ke semua /ws/stream client via thread-safe scheduler.

    Penggunaan di reconstruct_server.py:
        from apps.dashboard_server import notify_window
        notify_window(node_id, window_num, ts, hr, spo2, finger, report, elapsed_ms)
    """
    # Ringkas metrik per sinyal (tanpa array values — besar)
    signals_quality: dict = {}
    if report is not None:
        for sig, m in report.metrics.items():
            snr = m.snr_db
            signals_quality[sig] = {
                "rel_error": round(m.relative_error, 4),
                "flag":      m.flag.value,
                "sparsity":  round(m.sparsity_ratio, 3),
                "snr_db":    round(snr, 1) if snr != float("inf") else 999.0,
            }

    data = {
        "type":       "window",
        "node_id":    node_id,
        "window_num": window_num,
        "ts":         ts,
        "hr":         hr,
        "spo2":       spo2,
        "finger":     finger,
        "elapsed_ms": round(elapsed_ms, 1),
        "quality": {
            "avg_rel_error":   round(report.mean_relative_error(), 4) if report else None,
            "any_low_quality": report.has_low_quality() if report else False,
            "any_critical":    report.has_critical()    if report else False,
            "signals":         signals_quality,
        },
    }

    # Update statistik server
    _server_stats["total_windows"]    += 1
    _server_stats["total_rekon_ms"]   += elapsed_ms
    if report and report.has_critical():
        _server_stats["total_critical"] += 1
    elif report and report.has_low_quality():
        _server_stats["total_low_quality"] += 1

    hub.publish_window_threadsafe(data)


def notify_event(
    node_id:    int,
    event_type: str,
    detail:     str,
) -> None:
    """
    Dipanggil dari reconstruct_server saat ada event anomali atau validasi gagal.
    Push ke semua /ws/events client.

    Penggunaan di reconstruct_server.py:
        from apps.dashboard_server import notify_event
        notify_event(node_id, "LOW_QUALITY", "win=5 signals=gx gz")
    """
    data = {
        "type":       "event",
        "node_id":    node_id,
        "event_type": event_type,
        "detail":     detail,
        "ts_ms":      _now_ms(),
    }

    if event_type == "VALIDATION_ERROR":
        _server_stats["total_val_errors"] += 1

    hub.publish_event_threadsafe(data)


# =============================================================================
# Entry point
# =============================================================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "apps.dashboard_server:app",
        host    = "0.0.0.0",
        port    = 8000,
        reload  = False,
        workers = 1,   # WAJIB 1 agar hub & storage tidak ter-fork
    )
