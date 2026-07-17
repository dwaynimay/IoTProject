"""
adapter.py — Jembatan antara pipeline rekonstruksi dan ML engine.

Tanggung jawab:
    Konversi output dari processor.py (dict results + metadata)
    menjadi WindowInput yang siap di-infer engine.

Tidak ada logika bisnis di sini — hanya mapping data.
"""

from __future__ import annotations

import logging
from typing import Optional

import numpy as np

from .schemas import WindowInput

logger = logging.getLogger(__name__)


def from_processor(
    *,
    node_id:    int,
    window_num: int,
    imu_data:   dict,
    ppg_data:   dict,
    results:    "dict[str, np.ndarray]",
) -> WindowInput:
    """
    Buat WindowInput dari output processor.process_window().

    Args:
        node_id    : ID node
        window_num : nomor window
        imu_data   : payload cs_imu asli (untuk metadata ts, finger)
        ppg_data   : payload cs_ppg asli (untuk metadata hr, spo2, finger)
        results    : dict sinyal → ndarray rekonstruksi dari processor

    Returns:
        WindowInput siap di-infer.
    """
    def _to_list(sig: str) -> Optional[list[float]]:
        arr = results.get(sig)
        if arr is None:
            return None
        # Convert accelerometer signals from m/s^2 to Gs (gravity units)
        # to match model training dataset (UMA ADL dataset in Gs)
        if sig in ["ax", "ay", "az"]:
            arr = arr / 9.80665
        # Convert PPG signals to match model training dataset baseline (around 600-700)
        elif sig == "ir":
            arr = arr / 180.88
        return arr.tolist() if isinstance(arr, np.ndarray) else list(arr)

    hr     = ppg_data.get("hr", -1)
    spo2   = ppg_data.get("spo2", None)
    finger = bool(ppg_data.get("finger", False) or imu_data.get("finger", False))
    ts     = imu_data.get("ts", 0)

    return WindowInput(
        node_id    = node_id,
        window_num = window_num,
        ts         = ts,
        ax = _to_list("ax"),
        ay = _to_list("ay"),
        az = _to_list("az"),
        gx = _to_list("gx"),
        gy = _to_list("gy"),
        gz = _to_list("gz"),
        ir = _to_list("ir"),
        hr     = int(hr) if hr is not None else -1,
        spo2   = float(spo2) if spo2 is not None else None,
        finger = finger,
    )


def from_storage_rows(
    *,
    node_id:    int,
    window_num: int,
    ts:         int,
    signal_rows: "dict[str, list[float]]",
    hr:         int   = -1,
    spo2:       Optional[float] = None,
    finger:     bool  = False,
) -> WindowInput:
    """
    Buat WindowInput dari baris SQLite (get_last_windows).

    Args:
        node_id     : ID node
        window_num  : nomor window
        ts          : timestamp sensor (ms)
        signal_rows : dict sinyal → list float (dari storage.get_last_windows per sinyal)
        hr, spo2, finger : metadata dari row IR

    Contoh pemakaian (batch dari SQLite):
        from core.storage import StorageManager
        from apps.ml_inference.adapter import from_storage_rows

        db = StorageManager(...)
        db.open()

        # Kumpulkan semua sinyal untuk window tertentu
        signals = {}
        for sig in ["ax","ay","az","gx","gy","gz","ir"]:
            rows = db.get_last_windows(node_id=1, signal=sig, n=1)
            if rows:
                signals[sig] = rows[0]["values"]  # list float panjang 64
                # ambil metadata dari row IR
                if sig == "ir":
                    hr     = rows[0].get("hr", -1)
                    spo2   = rows[0].get("spo2")
                    finger = rows[0].get("finger", False)
                    ts     = rows[0].get("ts_sensor_ms", 0)
                    wn     = rows[0].get("window_num", 0)

        window_input = from_storage_rows(
            node_id=1, window_num=wn, ts=ts,
            signal_rows=signals, hr=hr, spo2=spo2, finger=finger
        )
    """
    def _to_g(sig: str) -> Optional[list[float]]:
        vals = signal_rows.get(sig)
        if vals is None:
            return None
        # Convert accelerometer signals from m/s^2 to Gs
        if sig in ["ax", "ay", "az"]:
            return [v / 9.80665 for v in vals]
        # Convert PPG signals to match model training dataset baseline
        if sig == "ir":
            return [v / 180.88 for v in vals]
        return vals

    return WindowInput(
        node_id    = node_id,
        window_num = window_num,
        ts         = ts,
        ax = _to_g("ax"),
        ay = _to_g("ay"),
        az = _to_g("az"),
        gx = signal_rows.get("gx"),
        gy = signal_rows.get("gy"),
        gz = signal_rows.get("gz"),
        ir = signal_rows.get("ir"),
        hr     = hr,
        spo2   = spo2,
        finger = finger,
    )
