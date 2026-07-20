"""Per-group modality pairing and ordered processor dispatch."""

from __future__ import annotations

from collections import deque
import logging
import queue
import threading
import time
from typing import Callable

from core import QualityAssessor, StorageManager, ValidatorRegistry
from core.config import (
    PAIR_BUFFER_SIZE,
    PROCESSOR_QUEUE_SIZE,
    TS_SPREAD_TOLERANCE_MS,
)
from .notifier import notify_event

logger = logging.getLogger(__name__)


class NodeState:
    """Pair IMU/PPG arrivals and process each group in strict FIFO order."""

    _BUFFER_STALE_MS = 3000

    def __init__(
        self,
        group_id: int,
        imu_node_id: int,
        ppg_node_id: int,
        processor_fn: Callable,
        validator: "ValidatorRegistry",
        assessor: "QualityAssessor",
        storage: "StorageManager",
    ) -> None:
        self.group_id = group_id
        self.imu_node_id = imu_node_id
        self.ppg_node_id = ppg_node_id
        self._processor = processor_fn
        self._validator = validator
        self._assessor = assessor
        self._storage = storage

        self._imu_buffers: deque[tuple[dict, float]] = deque()
        self._ppg_buffers: deque[tuple[dict, float]] = deque()
        self._lock = threading.Lock()
        self._work_queue: queue.Queue[tuple[int, dict, dict]] = queue.Queue(
            maxsize=PROCESSOR_QUEUE_SIZE
        )

        self.windows_done = 0
        self.windows_succeeded = 0
        self.windows_failed = 0
        self.windows_dropped = 0
        self._timing: dict = {}

        self._worker = threading.Thread(
            target=self._worker_loop,
            name=f"processor-group-{group_id}",
            daemon=True,
        )
        self._worker.start()

    def on_imu(self, payload: dict) -> None:
        ok, errors = self._validator.validate_imu(
            node_id=self.imu_node_id, payload=payload
        )
        if not ok:
            self._record_event("VALIDATION_ERROR", f"cs_imu: {'; '.join(errors)[:400]}")
            return
        self._accept("imu", payload)

    def on_ppg(self, payload: dict) -> None:
        ok, errors = self._validator.validate_ppg(
            node_id=self.ppg_node_id, payload=payload
        )
        if not ok:
            self._record_event("VALIDATION_ERROR", f"cs_ppg: {'; '.join(errors)[:400]}")
            return
        self._accept("ppg", payload)

    def _accept(self, modality: str, payload: dict) -> None:
        now = time.monotonic()
        events: list[tuple[str, str]] = []
        with self._lock:
            events.extend(self._expire_stale_locked(now))
            buffers = self._imu_buffers if modality == "imu" else self._ppg_buffers
            if len(buffers) >= PAIR_BUFFER_SIZE:
                buffers.popleft()
                self.windows_dropped += 1
                events.append((
                    "PAIR_BUFFER_OVERFLOW",
                    f"modality={modality} limit={PAIR_BUFFER_SIZE}",
                ))
            buffers.append((payload, now))
            events.extend(self._pair_locked())

        for event_type, detail in events:
            self._record_event(event_type, detail)

    def _pair_locked(self) -> list[tuple[str, str]]:
        events: list[tuple[str, str]] = []
        while self._imu_buffers and self._ppg_buffers:
            imu_data, imu_arrival = self._imu_buffers[0]
            ppg_data, ppg_arrival = self._ppg_buffers[0]
            arrival_spread_ms = abs(imu_arrival - ppg_arrival) * 1000.0

            if arrival_spread_ms > TS_SPREAD_TOLERANCE_MS:
                if imu_arrival < ppg_arrival:
                    dropped = "imu"
                    self._imu_buffers.popleft()
                else:
                    dropped = "ppg"
                    self._ppg_buffers.popleft()
                self.windows_dropped += 1
                events.append((
                    "PAIR_MISMATCH",
                    f"dropped={dropped} arrival_spread_ms={arrival_spread_ms:.1f}",
                ))
                continue

            self._imu_buffers.popleft()
            self._ppg_buffers.popleft()
            window_num = self.windows_done + 1
            try:
                self._work_queue.put_nowait((window_num, imu_data, ppg_data))
            except queue.Full:
                self.windows_dropped += 1
                events.append((
                    "PROCESSOR_OVERLOAD",
                    f"window={window_num} queue_limit={PROCESSOR_QUEUE_SIZE}",
                ))
                continue
            self.windows_done = window_num

        return events

    def _expire_stale_locked(self, now: float) -> list[tuple[str, str]]:
        events: list[tuple[str, str]] = []
        stale_s = self._BUFFER_STALE_MS / 1000.0
        for modality, buffers in (
            ("imu", self._imu_buffers),
            ("ppg", self._ppg_buffers),
        ):
            while buffers and now - buffers[0][1] > stale_s:
                age_s = now - buffers[0][1]
                buffers.popleft()
                self.windows_dropped += 1
                events.append((
                    "PAIR_EXPIRED",
                    f"modality={modality} age_s={age_s:.1f}",
                ))
        return events

    def _worker_loop(self) -> None:
        while True:
            try:
                job = self._work_queue.get(timeout=0.5)
            except queue.Empty:
                with self._lock:
                    events = self._expire_stale_locked(time.monotonic())
                for event_type, detail in events:
                    self._record_event(event_type, detail)
                continue

            window_num, imu_data, ppg_data = job
            try:
                self._processor(
                    node_id=self.group_id,
                    window_num=window_num,
                    imu_data=imu_data,
                    ppg_data=ppg_data,
                    assessor=self._assessor,
                    storage=self._storage,
                    timing=self._timing,
                )
                self.windows_succeeded += 1
            except Exception as exc:
                self.windows_failed += 1
                detail = f"window={window_num} error={type(exc).__name__}: {exc}"
                logger.exception("Group %d | processor error: %s", self.group_id, exc)
                self._record_event("PROCESSOR_ERROR", detail[:400])
            finally:
                self._work_queue.task_done()

    def _record_event(self, event_type: str, detail: str) -> None:
        logger.warning("Group %d | %s | %s", self.group_id, event_type, detail)
        try:
            self._storage.log_event(self.group_id, event_type, detail)
        except Exception:
            logger.exception("Group %d | gagal menyimpan event %s", self.group_id, event_type)
        try:
            notify_event(self.group_id, event_type, detail)
        except Exception:
            logger.exception("Group %d | gagal mengirim event %s", self.group_id, event_type)
