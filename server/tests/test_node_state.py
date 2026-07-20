import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import apps.reconstruct.node_state as node_state_module
from apps.reconstruct.node_state import NodeState


class _ValidValidator:
    def validate_imu(self, node_id, payload):
        return True, []

    def validate_ppg(self, node_id, payload):
        return True, []


class _Storage:
    def __init__(self):
        self.events = []

    def log_event(self, node_id, event_type, detail=""):
        self.events.append((node_id, event_type, detail))


class _FailingStorage:
    def log_event(self, node_id, event_type, detail=""):
        raise RuntimeError("database unavailable")


def _make_node(processor, storage=None):
    storage = storage or _Storage()
    return NodeState(
        group_id=7,
        imu_node_id=1,
        ppg_node_id=2,
        processor_fn=processor,
        validator=_ValidValidator(),
        assessor=object(),
        storage=storage,
    ), storage


def test_multiple_pairs_are_processed_fifo_without_overwrite():
    processed = []

    def processor(**kwargs):
        processed.append((kwargs["window_num"], kwargs["imu_data"]["seq"],
                          kwargs["ppg_data"]["seq"]))

    node, _ = _make_node(processor)
    node.on_imu({"seq": 1})
    node.on_imu({"seq": 2})
    node.on_ppg({"seq": 1})
    node.on_ppg({"seq": 2})
    node._work_queue.join()

    assert processed == [(1, 1, 1), (2, 2, 2)]
    assert node.windows_succeeded == 2


def test_processor_exception_is_persisted_and_worker_survives():
    storage = _Storage()
    attempts = []

    def processor(**kwargs):
        attempts.append(kwargs["window_num"])
        if kwargs["window_num"] == 1:
            raise RuntimeError("boom")

    node, _ = _make_node(processor, storage)
    for seq in (1, 2):
        node.on_imu({"seq": seq})
        node.on_ppg({"seq": seq})
    node._work_queue.join()

    assert attempts == [1, 2]
    assert node.windows_failed == 1
    assert node.windows_succeeded == 1
    assert any(event[1] == "PROCESSOR_ERROR" for event in storage.events)


def test_event_storage_failure_does_not_kill_worker():
    attempts = []

    def processor(**kwargs):
        attempts.append(kwargs["window_num"])
        if kwargs["window_num"] == 1:
            raise RuntimeError("boom")

    node, _ = _make_node(processor, _FailingStorage())
    for seq in (1, 2):
        node.on_imu({"seq": seq})
        node.on_ppg({"seq": seq})
    node._work_queue.join()

    assert attempts == [1, 2]
    assert node.windows_failed == 1
    assert node.windows_succeeded == 1


def test_stale_unpaired_payload_expires_without_new_arrival():
    node, storage = _make_node(lambda **kwargs: None)
    node._BUFFER_STALE_MS = 10
    node.on_imu({"seq": 1})

    deadline = time.monotonic() + 1.2
    while time.monotonic() < deadline:
        if any(event[1] == "PAIR_EXPIRED" for event in storage.events):
            break
        time.sleep(0.02)

    assert any(event[1] == "PAIR_EXPIRED" for event in storage.events)
    assert node.windows_dropped == 1


def test_processor_queue_overload_is_reported(monkeypatch):
    monkeypatch.setattr(node_state_module, "PROCESSOR_QUEUE_SIZE", 1)
    release = threading.Event()
    started = threading.Event()

    def processor(**kwargs):
        started.set()
        release.wait(timeout=2)

    node, storage = _make_node(processor)
    node.on_imu({"seq": 1})
    node.on_ppg({"seq": 1})
    assert started.wait(timeout=1)

    node.on_imu({"seq": 2})
    node.on_ppg({"seq": 2})
    node.on_imu({"seq": 3})
    node.on_ppg({"seq": 3})

    assert any(event[1] == "PROCESSOR_OVERLOAD" for event in storage.events)
    assert node.windows_dropped == 1
    release.set()
    node._work_queue.join()
