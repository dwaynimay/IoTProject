# File: server/apps/dashboard/routes/metrics.py

import time
from fastapi import APIRouter

from apps.dashboard.hub import hub, storage, server_stats, server_stats_lock

router = APIRouter(tags=["Overview"])


def _now_ms() -> int:
    return int(time.time() * 1000)


@router.get(
    "/api/metrics",
    summary="Metrik server keseluruhan",
)
def get_metrics():
    """
    Statistik throughput dan kualitas sejak server start.
    Diupdate real-time oleh `notify_window()` dari reconstruct_server.
    """
    with server_stats_lock:
        stats = dict(server_stats)

    total_w  = max(stats["total_windows"], 1)
    avg_ms   = stats["total_rekon_ms"] / total_w
    uptime_s = (_now_ms() - server_stats["start_time_ms"]) / 1000

    return {
        "uptime_s":            round(uptime_s, 1),
        "total_windows":       stats["total_windows"],
        "avg_rekon_ms":        round(avg_ms, 2),
        "total_val_errors":    stats["total_val_errors"],
        "total_low_quality":   stats["total_low_quality"],
        "total_critical":      stats["total_critical"],
        "val_error_rate":      round(stats["total_val_errors"] / total_w, 4),
        "low_quality_rate":    round(stats["total_low_quality"] / total_w, 4),
        "ws_stream_clients":   hub.stream_count,
        "ws_event_clients":    hub.event_count,
    }


@router.get(
    "/api/db",
    summary="Info database SQLite",
)
def get_db_info():
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
