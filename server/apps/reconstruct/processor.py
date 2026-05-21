"""
processor.py — Logika rekonstruksi, quality assessment, storage, dan dashboard push.

Tanggung jawab tunggal:
    Menerima imu_data + ppg_data yang sudah divalidasi, merekonstruksi sinyal,
    menilai kualitas, menyimpan ke SQLite, dan meneruskan ke dashboard WebSocket.

Tidak ada MQTT di sini. Tidak ada state per-node di sini.
"""

import logging
import time

import numpy as np

from core import (
    reconstruct, PHI,
    QualityAssessor, StorageManager,
    CS_M, IMU_SIGNALS,
)

logger = logging.getLogger(__name__)

# ── Konstanta ──────────────────────────────────────────────────────────────────

PURGE_EVERY_WINDOWS = 100
_global_window_count = 0

import threading
_global_lock = threading.Lock()


# ── Dashboard notifier (opsional) ─────────────────────────────────────────────

try:
    from apps.dashboard_server import notify_window as _notify_window
    from apps.dashboard_server import notify_event  as _notify_event
    _DASHBOARD_AVAILABLE = True
except ImportError:
    _DASHBOARD_AVAILABLE = False
    def _notify_window(*args, **kwargs): pass  # noqa: E301
    def _notify_event(*args, **kwargs):  pass  # noqa: E301


# ── Public API ────────────────────────────────────────────────────────────────

def process_window(
    node_id:    int,
    window_num: int,
    imu_data:   dict,
    ppg_data:   dict,
    *,
    assessor:  "QualityAssessor",
    storage:   "StorageManager",
    timing:    dict,
) -> None:
    """
    Rekonstruksi satu window CS, nilai kualitas, simpan, dan push ke dashboard.

    Args:
        node_id    : ID node pengirim
        window_num : nomor window (dikelola NodeState)
        imu_data   : payload cs_imu yang sudah lolos validasi
        ppg_data   : payload cs_ppg yang sudah lolos validasi
        assessor   : instance QualityAssessor (singleton dari __main__)
        storage    : instance StorageManager  (singleton dari __main__)
        timing     : dict mutable untuk statistik waktu per node
                     keys: 'total_rec_ms', 'last_win_t'
    """
    global _global_window_count

    results:      dict[str, np.ndarray] = {}
    measurements: dict[str, list]       = {}
    t0 = time.time()

    # ── Rekonstruksi IMU ──────────────────────────────────────────────────────
    for sig in IMU_SIGNALS:
        y = imu_data.get(sig, [])
        if len(y) == CS_M:
            results[sig]      = reconstruct(y)
            measurements[sig] = y
        else:
            logger.warning("Node %d | %s: len=%d expected %d — skip",
                           node_id, sig, len(y), CS_M)

    # ── Rekonstruksi PPG ──────────────────────────────────────────────────────
    y_ir = ppg_data.get("ir", [])
    if len(y_ir) == CS_M:
        results["ir"]      = reconstruct(y_ir)
        measurements["ir"] = y_ir

    elapsed_ms = (time.time() - t0) * 1000

    # ── Statistik timing ──────────────────────────────────────────────────────
    timing["total_rec_ms"] = timing.get("total_rec_ms", 0.0) + elapsed_ms
    avg_ms  = timing["total_rec_ms"] / window_num
    now     = time.time()
    gap_ms  = (now - timing.get("last_win_t", now)) * 1000
    timing["last_win_t"] = now

    # ── Metadata dari payload ─────────────────────────────────────────────────
    hr     = ppg_data.get("hr", -1)
    spo2   = ppg_data.get("spo2", None)
    finger = ppg_data.get("finger", False)
    ts     = imu_data.get("ts", 0)

    # ── F3: Quality assessment ────────────────────────────────────────────────
    report = assessor.assess_window(
        results      = results,
        measurements = measurements,
        node_id      = node_id,
        window_num   = window_num,
    )

    # ── F6: Simpan ke SQLite ──────────────────────────────────────────────────
    storage.save_window(
        node_id    = node_id,
        window_num = window_num,
        ts_sensor  = ts,
        results    = results,
        report     = report,
        hr         = hr if hr is not None else -1,
        spo2       = spo2 if spo2 is not None else 0.0,
        finger     = finger,
    )

    # ── F4: Push ke dashboard WebSocket ──────────────────────────────────────
    _notify_window(
        node_id    = node_id,
        window_num = window_num,
        ts         = ts,
        hr         = hr if hr is not None else -1,
        spo2       = spo2,
        finger     = finger,
        report     = report,
        elapsed_ms = elapsed_ms,
    )

    # ── Log anomali ke storage + dashboard ───────────────────────────────────
    if report.has_critical():
        sigs   = " ".join(report.critical_signals())
        detail = f"win={window_num} signals={sigs}"
        storage.log_event(node_id, "CRITICAL", detail)
        _notify_event(node_id, "CRITICAL", detail)
    elif report.has_low_quality():
        sigs   = " ".join(report.low_quality_signals())
        detail = f"win={window_num} signals={sigs}"
        storage.log_event(node_id, "LOW_QUALITY", detail)
        _notify_event(node_id, "LOW_QUALITY", detail)

    # ── Purge periodik ────────────────────────────────────────────────────────
    with _global_lock:
        _global_window_count += 1
        do_purge = (_global_window_count % PURGE_EVERY_WINDOWS == 0)
    if do_purge:
        storage.purge_old()

    # ── Console log ───────────────────────────────────────────────────────────
    spo2_str = f" | SpO2={spo2:.1f}%" if spo2 is not None else ""
    q_tag    = ""
    if report.has_critical():
        q_tag = " ⚠ CRITICAL"
    elif report.has_low_quality():
        q_tag = " ⚠ LOW_Q"

    logger.info(
        "\n[Node %d] Window #%d | ts=%dms | gap=%.0fms "
        "| HR=%s%s | finger=%s | rekon=%.1fms | avg=%.1fms%s",
        node_id, window_num, ts, gap_ms,
        hr, spo2_str, "Y" if finger else "N",
        elapsed_ms, avg_ms, q_tag,
    )
    logger.info("  %s", report.summary())
    for line in report.detail_lines():
        logger.debug("  %s", line)

    if window_num % 20 == 0:
        logger.info("  %s", assessor.stats_summary())
    if window_num % 50 == 0:
        size_kb = storage.db_size_bytes() / 1024
        logger.info("  [Storage] DB size=%.1f KB", size_kb)
