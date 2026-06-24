"""
node_state.py — State management per node group: buffer imu/ppg, timestamp spread check.

Tanggung jawab tunggal:
    Menyimpan buffer cs_imu dan cs_ppg per group. Data bisa datang dari
    node fisik yang berbeda (1 sensor per ESP). Saat keduanya siap dan
    timestamp spread dalam toleransi, memanggil processor.process_window()
    via thread pool agar MQTT callback thread tidak diblokir.

Perubahan v4 (cross-node pairing):
    - Constructor menerima group_id, imu_node_id, ppg_node_id
    - Validasi monotonicity menggunakan node_id fisik (bukan group_id)
    - Output (storage, dashboard) menggunakan group_id
    - Backward-compatible: jika imu_node == ppg_node, behavior sama seperti v3

Tidak ada MQTT di sini. Tidak ada logika rekonstruksi di sini.
"""

import logging
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Callable

from core import ValidatorRegistry, StorageManager, QualityAssessor
from core.config import TS_SPREAD_TOLERANCE_MS

logger = logging.getLogger(__name__)

# Thread pool terpisah untuk proses berat (CS rekon + ML inference + DB write).
# max_workers=2: cukup untuk 2-3 node paralel tanpa membebani CPU.
_PROCESSOR_POOL = ThreadPoolExecutor(max_workers=2, thread_name_prefix="proc")


class NodeState:
    """
    Buffer dan state per node group.

    Mendukung dua mode:
      1. Same-node mode  : imu_node_id == ppg_node_id (2 sensor per 1 ESP, legacy)
      2. Cross-node mode : imu_node_id != ppg_node_id (1 sensor per ESP)

    Args:
        group_id     : ID virtual group (dipakai sebagai node_id output)
        imu_node_id  : node_id fisik pengirim cs_imu
        ppg_node_id  : node_id fisik pengirim cs_ppg
        processor_fn : callable process_window() dari processor.py
        validator    : instance ValidatorRegistry bersama
        assessor     : instance QualityAssessor bersama
        storage      : instance StorageManager bersama
    """

    def __init__(
        self,
        group_id:     int,
        imu_node_id:  int,
        ppg_node_id:  int,
        processor_fn: Callable,
        validator:    "ValidatorRegistry",
        assessor:     "QualityAssessor",
        storage:      "StorageManager",
    ) -> None:
        self.group_id     = group_id
        self.imu_node_id  = imu_node_id
        self.ppg_node_id  = ppg_node_id
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
        """Terima payload cs_imu, validasi dengan node_id fisik, simpan ke buffer."""
        ok, errors = self._validator.validate_imu(
            node_id=self.imu_node_id, payload=payload
        )
        if not ok:
            msg = "; ".join(errors)
            logger.warning("Group %d (imu_node=%d) | VALIDATION ERROR (cs_imu): %s",
                           self.group_id, self.imu_node_id, msg)
            self._storage.log_event(self.group_id, "VALIDATION_ERROR",
                                    f"cs_imu: {msg[:400]}")
            return

        with self._lock:
            self._imu_buf = payload
            self._try_dispatch()

    def on_ppg(self, payload: dict) -> None:
        """Terima payload cs_ppg, validasi dengan node_id fisik, simpan ke buffer."""
        ok, errors = self._validator.validate_ppg(
            node_id=self.ppg_node_id, payload=payload
        )
        if not ok:
            msg = "; ".join(errors)
            logger.warning("Group %d (ppg_node=%d) | VALIDATION ERROR (cs_ppg): %s",
                           self.group_id, self.ppg_node_id, msg)
            self._storage.log_event(self.group_id, "VALIDATION_ERROR",
                                    f"cs_ppg: {msg[:400]}")
            return

        with self._lock:
            self._ppg_buf = payload
            self._try_dispatch()

    # ── Internal ──────────────────────────────────────────────────────────────

    def _try_dispatch(self) -> None:
        """Cek apakah kedua buffer siap. Jika ya, submit ke thread pool."""
        if self._imu_buf is None or self._ppg_buf is None:
            return

        ts_imu = self._imu_buf.get("ts", 0)
        ts_ppg = self._ppg_buf.get("ts", 0)
        spread = abs(ts_imu - ts_ppg)

        if spread > TS_SPREAD_TOLERANCE_MS:
            # v3.2: Log warning saja, tetap dispatch (tidak drop window)
            logger.warning("Group %d | ts spread=%dms > %dms — accepting anyway "
                           "(imu_ts=%d ppg_ts=%d)",
                           self.group_id, spread, TS_SPREAD_TOLERANCE_MS,
                           ts_imu, ts_ppg)

        # Ambil dan kosongkan buffer — MQTT thread langsung bebas setelah ini
        imu_data, ppg_data = self._imu_buf, self._ppg_buf
        self._imu_buf = None
        self._ppg_buf = None

        self.windows_done += 1
        window_num = self.windows_done

        # Submit ke thread pool — proses berat tidak memblokir MQTT thread
        _PROCESSOR_POOL.submit(
            self._run_processor, window_num, imu_data, ppg_data
        )

    def _run_processor(self, window_num: int, imu_data: dict, ppg_data: dict) -> None:
        """Dijalankan di thread pool — boleh lambat tanpa memblokir MQTT."""
        try:
            self._processor(
                node_id    = self.group_id,  # output menggunakan group_id
                window_num = window_num,
                imu_data   = imu_data,
                ppg_data   = ppg_data,
                assessor   = self._assessor,
                storage    = self._storage,
                timing     = self._timing,
            )
        except Exception as exc:
            logger.error("Group %d | processor error: %s", self.group_id, exc,
                         exc_info=True)
