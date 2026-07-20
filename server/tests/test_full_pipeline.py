# File: server/tests/test_full_pipeline.py

import os
import sys
import tempfile
from fastapi.testclient import TestClient

# Pastikan server/ ada di sys.path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from apps.dashboard.hub import storage
from apps.dashboard.app import app


def test_integration_full_pipeline():
    """
    E2E Integration Test:
    Mulai dari payload mentah sensor -> validasi & reconstruction -> penyimpanan SQLite -> REST API.
    """
    # 1. Setup temporary database for test isolation
    tmp_db = tempfile.mktemp(suffix="_test.db")

    # Override storage in dashboard hub to use our temp DB
    from pathlib import Path
    storage.close()
    storage._path = Path(tmp_db)
    storage.open()

    client = TestClient(app)

    # Verifikasi DB bersih di awal
    response = client.get("/api/db")
    assert response.status_code == 200
    data = response.json()
    assert data["rows_windows"] == 0
    assert data["rows_events"] == 0

    # 2. Siapkan data sensor (IMU + PPG)
    from core import ValidatorRegistry, QualityAssessor, PHI
    from core.config import CS_M, IMU_SIGNALS, PPG_SIGNALS
    from apps.reconstruct.processor import process_window
    from apps.reconstruct.node_state import NodeState

    imu_payload = {"ts": 1000, "finger": True}
    for sig in IMU_SIGNALS:
        imu_payload[sig] = [0.01 * i for i in range(CS_M)]
        imu_payload[f"mean_{sig}"] = 0.0

    ppg_payload = {
        "ts": 1000, "hr": 75, "spo2": 98.0,
        "ppg_valid": True, "finger": True, "mean_ir": 100.0,
    }
    for sig in PPG_SIGNALS:
        ppg_payload[sig] = [100.0 + i for i in range(CS_M)]

    # 3. Jalankan pipeline (simulasi dari MQTT handler)
    validator = ValidatorRegistry()
    assessor  = QualityAssessor(phi=PHI)

    node = NodeState(
        group_id     = 1,
        imu_node_id  = 1,
        ppg_node_id  = 2,
        processor_fn = process_window,
        validator    = validator,
        assessor     = assessor,
        storage      = storage,
    )

    node.on_imu(imu_payload)
    node.on_ppg(ppg_payload)
    node._work_queue.join()

    # 4. Verifikasi REST API merefleksikan data terproses
    # Cek DB info
    response = client.get("/api/db")
    assert response.status_code == 200
    data = response.json()
    assert data["rows_windows"] > 0

    # Cek Status
    response = client.get("/api/status")
    assert response.status_code == 200
    data = response.json()
    assert len(data["nodes"]) == 1
    assert data["nodes"][0]["node_id"] == 1
    assert data["nodes"][0]["total_windows"] == 1
    assert data["nodes"][0]["last_hr"] == 75

    # Cek Detail Node
    response = client.get("/api/nodes/1")
    assert response.status_code == 200
    data = response.json()
    assert data["stats"]["node_id"] == 1
    assert "last_window" in data
    assert "ir" in data["last_window"]

    # Clean up
    storage.close()
    if os.path.exists(tmp_db):
        try:
            os.remove(tmp_db)
        except OSError:
            pass


if __name__ == "__main__":
    try:
        test_integration_full_pipeline()
        print("Integration Test: PASSED")
    except Exception as e:
        print(f"Integration Test: FAILED ({e})")
        raise e
