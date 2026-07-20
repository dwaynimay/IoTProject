# File: server/apps/reconstruct/notifier.py

import logging
import time
from typing import Optional

logger = logging.getLogger(__name__)

# Try importing dashboard components for single-process integration
try:
    from apps.dashboard.hub import hub, server_stats, server_stats_lock
    _DASHBOARD_AVAILABLE = True
except ImportError:
    _DASHBOARD_AVAILABLE = False
    hub = None
    server_stats = None
    server_stats_lock = None


def notify_window(
    node_id:    int,
    window_num: int,
    ts:         int,
    hr:         int,
    spo2:       Optional[float],
    finger:     bool,
    report,                         # WindowReport dari quality.py
    elapsed_ms: float,
    ml_results: dict = None,
    results:    dict = None,        # dict sig -> np.ndarray hasil rekonstruksi
) -> None:
    """
    Dipanggil setelah window selesai direkonstruksi.
    Push payload ke semua /ws/stream client via thread-safe scheduler.
    """
    if not _DASHBOARD_AVAILABLE or hub is None:
        return

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

    # Serialize sinyal IMU ke array float (dibulatkan untuk hemat bandwidth)
    _IMU_KEYS = ["ax", "ay", "az", "gx", "gy", "gz"]
    imu_signals: dict = {}
    if results:
        for sig in _IMU_KEYS:
            if sig in results:
                imu_signals[sig] = [round(float(v), 3) for v in results[sig]]

    data = {
        "type":        "window",
        "node_id":     node_id,
        "window_num":  window_num,
        "ts":          ts,
        "hr":          hr,
        "spo2":        spo2,
        "finger":      finger,
        "elapsed_ms":  round(elapsed_ms, 1),
        "quality": {
            "avg_rel_error":   round(report.mean_relative_error(), 4) if report else None,
            "any_low_quality": report.has_low_quality() if report else False,
            "any_critical":    report.has_critical()    if report else False,
            "signals":         signals_quality,
        },
        "ml_results":  ml_results if ml_results is not None else {},
        "imu_signals": imu_signals,
    }

    # Update statistik server
    if server_stats is not None and server_stats_lock is not None:
        with server_stats_lock:
            server_stats["total_windows"]    += 1
            server_stats["total_rekon_ms"]   += elapsed_ms
            if report and report.has_critical():
                server_stats["total_critical"] += 1
            elif report and report.has_low_quality():
                server_stats["total_low_quality"] += 1

    hub.publish_window_threadsafe(data)


def notify_event(
    node_id:    int,
    event_type: str,
    detail:     str,
) -> None:
    """
    Dipanggil saat ada event anomali atau validasi gagal.
    Push ke semua /ws/events client.
    """
    if not _DASHBOARD_AVAILABLE or hub is None:
        return

    data = {
        "type":       "event",
        "node_id":    node_id,
        "event_type": event_type,
        "detail":     detail,
        "ts_ms":      int(time.time() * 1000),
    }

    if (event_type == "VALIDATION_ERROR" and server_stats is not None
            and server_stats_lock is not None):
        with server_stats_lock:
            server_stats["total_val_errors"] += 1

    hub.publish_event_threadsafe(data)
