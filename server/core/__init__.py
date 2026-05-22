"""
Public API server core.

apps/ HANYA boleh import dari sini, bukan dari submodul langsung.

Contoh penggunaan:
    from server.core import reconstruct, PHI, THETA, PSI
    from server.core import QualityAssessor, ValidatorRegistry, StorageManager
"""

from cs import reconstruct, PHI, THETA, PSI, ALGORITHM_NAME
from .quality   import QualityAssessor, QualityFlag, SignalMetric, WindowReport
from .validator import ValidatorRegistry, ValidationError
from .storage    import StorageManager
from .config     import (
    CS_N, CS_M, OMP_K, CS_PHI_SEED,
    IMU_SIGNALS, PPG_SIGNALS, SIGNALS,
    MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, TOPIC_BASE,
    DB_PATH, RETENTION_HOURS, LOG_LEVEL,
)

__all__ = [
    # CS
    "reconstruct", "PHI", "THETA", "PSI", "ALGORITHM_NAME",
    # Quality
    "QualityAssessor", "QualityFlag", "SignalMetric", "WindowReport",
    # Validation
    "ValidatorRegistry", "ValidationError",
    # Storage
    "StorageManager",
    # Config
    "CS_N", "CS_M", "OMP_K", "CS_PHI_SEED",
    "IMU_SIGNALS", "PPG_SIGNALS", "SIGNALS",
    "MQTT_BROKER", "MQTT_PORT", "MQTT_KEEPALIVE", "TOPIC_BASE",
    "DB_PATH", "RETENTION_HOURS", "LOG_LEVEL",
]
