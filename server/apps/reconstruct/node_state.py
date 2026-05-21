"""
node_state.py — State management per node: buffer imu/ppg, timestamp spread check.

Tanggung jawab tunggal:
    Menyimpan buffer cs_imu dan cs_ppg per node. Saat keduanya siap dan timestamp
    spread dalam toleransi, memanggil processor.process_window().

Tidak ada MQTT di sini. Tidak ada logika rekonstruksi di sini.
"""

import logging
import threading
from typing import Callable

from core import ValidatorRegistry, StorageManager, QualityAssessor
from core.config import TS_SPREAD_TOLERANCE_MS

logger = logging.getLogger(__name__)


class NodeState:
    """
    Buffer dan state per node sensor.

    Args:
        node_id      : ID node (dari MQTT topic, contoh: node_1 → 1)
        processor_fn : callable process_window() dari processor.py
        validator    : instance ValidatorRegistry bersama
        assessor     : instance QualityAssessor bersama
        storage      : instance StorageManager bersama
    """

    def __init__(
        self,
        node_id:      int,
        processor_fn: Callable,
        validator:    "ValidatorRegistry",
        assessor:     "QualityAssessor",
        storage:      "StorageManager",
    ) -> None:
        self.node_id      = node_id
        self._processor   = processor_fn
        self._validator   = validator
        self._assessor    = assessor
        self._storage     = storage

        self._imu_buf: dict | None = None
        self._ppg_buf: dict | None = None
        self._lock        = threading.Lock()

        self.windows_done = 0
        self._timing: dict = {}  # dict mutable untuk timing stats (lihat processor.py)

    # ── Public API ────────────────────────────────────────────────────────────

    def on_imu(self, payload: dict) -> None:
        """Terima payload cs_imu, validasi, simpan ke buffer."""
        ok, errors = self._validator.validate_imu(
            node_id=self.node_id, payload=payload
        )
        if not ok:
            msg = "; ".join(errors)
            logger.warning("Node %d | VALIDATION ERROR (cs_imu): %s",
                           self.node_id, msg)
            self._storage.log_event(self.node_id, "VALIDATION_ERROR",
                                    f"cs_imu: {msg[:400]}")
            return

        with self._lock:
            self._imu_buf = payload
            self._try_dispatch()

    def on_ppg(self, payload: dict) -> None:
        """Terima payload cs_ppg, validasi, simpan ke buffer."""
        ok, errors = self._validator.validate_ppg(
            node_id=self.node_id, payload=payload
        )
        if not ok:
            msg = "; ".join(errors)
            logger.warning("Node %d | VALIDATION ERROR (cs_ppg): %s",
                           self.node_id, msg)
            self._storage.log_event(self.node_id, "VALIDATION_ERROR",
                                    f"cs_ppg: {msg[:400]}")
            return

        with self._lock:
            self._ppg_buf = payload
            self._try_dispatch()

    # ── Internal ──────────────────────────────────────────────────────────────

    def _try_dispatch(self) -> None:
        """Cek apakah kedua buffer siap. Jika ya, dispatch ke processor."""
        if self._imu_buf is None or self._ppg_buf is None:
            return

        ts_imu = self._imu_buf.get("ts", 0)
        ts_ppg = self._ppg_buf.get("ts", 0)
        spread = abs(ts_imu - ts_ppg)

        if spread > TS_SPREAD_TOLERANCE_MS:
            if ts_imu < ts_ppg:
                logger.warning("Node %d | ts spread=%dms > %dms — reset imu buf",
                               self.node_id, spread, TS_SPREAD_TOLERANCE_MS)
                self._imu_buf = None
            else:
                logger.warning("Node %d | ts spread=%dms > %dms — reset ppg buf",
                               self.node_id, spread, TS_SPREAD_TOLERANCE_MS)
                self._ppg_buf = None
            return

        # Kedua buffer valid dan dalam toleransi — ambil dan kosongkan
        imu_data, ppg_data = self._imu_buf, self._ppg_buf
        self._imu_buf = None
        self._ppg_buf = None

        self.windows_done += 1

        self._processor(
            node_id    = self.node_id,
            window_num = self.windows_done,
            imu_data   = imu_data,
            ppg_data   = ppg_data,
            assessor   = self._assessor,
            storage    = self._storage,
            timing     = self._timing,
        )
