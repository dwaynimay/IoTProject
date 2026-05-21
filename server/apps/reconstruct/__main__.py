"""
Entry point: python -m apps.reconstruct

Inisialisasi singleton (validator, assessor, storage), lalu jalankan MQTT listener.
"""

import logging
import sys
import warnings

from core import (
    ValidatorRegistry, QualityAssessor, StorageManager, PHI,
    CS_N, CS_M, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE,
    TOPIC_BASE, DB_PATH, RETENTION_HOURS, LOG_LEVEL,
)
from core.logger import setup_logging

from .listener   import run as run_listener
from .processor  import process_window


def main() -> None:
    """Setup logging, singleton, lalu jalankan listener (blocking)."""
    warnings.filterwarnings("ignore", category=RuntimeWarning)

    setup_logging()
    logger = logging.getLogger(__name__)

    # ── Singleton shared ──────────────────────────────────────────────────────
    validator = ValidatorRegistry()
    assessor  = QualityAssessor(phi=PHI)
    storage   = StorageManager(db_path=DB_PATH, retention_hours=RETENTION_HOURS)

    storage.open()

    # ── Banner ────────────────────────────────────────────────────────────────
    print("=" * 60)
    print("  CS Reconstruction Server (F1 + F3 + F4 + F6)")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%) | OMP K=20")
    print(f"  Broker : {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  DB     : {DB_PATH} (retention={RETENTION_HOURS}h)")
    print("=" * 60)

    nodes: dict = {}

    try:
        run_listener(
            nodes        = nodes,
            broker       = MQTT_BROKER,
            port         = MQTT_PORT,
            keepalive    = MQTT_KEEPALIVE,
            topic_base   = TOPIC_BASE,
            storage      = storage,
            processor_fn = process_window,
            validator    = validator,
            assessor     = assessor,
        )
    except ConnectionRefusedError:
        logger.critical("Tidak bisa konek ke %s:%d — ConnectionRefused",
                        MQTT_BROKER, MQTT_PORT)
        storage.close()
        sys.exit(1)
    except KeyboardInterrupt:
        logger.info("Dihentikan oleh user.")
    finally:
        storage.close()
        logger.info("Storage ditutup.")


if __name__ == "__main__":
    main()
