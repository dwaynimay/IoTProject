"""
main_app.py — Orkestrator satu proses: FastAPI + MQTT worker dalam satu perintah.

Arsitektur:
    python -m server
        └── uvicorn (main thread / asyncio)
                ├── lifespan startup
                │       ├── storage.open()
                │       ├── hub.set_loop(asyncio event loop)
                │       └── Thread(target=_run_mqtt_thread).start()
                │
                ├── FastAPI routes (REST API + WebSocket)
                │       sama persis dengan apps/dashboard/app.py
                │
                └── [background] mqtt-worker thread
                        ├── listener.run() → blocking loop_forever()
                        └── per pesan masuk:
                                NodeState → processor → hub.publish_window_threadsafe()

Thread safety:
    StorageManager  → threading.Lock internal di storage.py
    _nodes dict     → threading.Lock di sini
    NodeState bufs  → threading.Lock per node (di node_state.py)
    WebSocket push  → asyncio.run_coroutine_threadsafe via hub
"""

from __future__ import annotations

import logging
import threading
from contextlib import asynccontextmanager
from pathlib import Path

import asyncio
import uvicorn
from fastapi import FastAPI

from core import (
    ValidatorRegistry, QualityAssessor, PHI,
    MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE,
    TOPIC_BASE, DB_PATH, RETENTION_HOURS,
    CS_N, CS_M,
)
from core.logger import setup_logging

# Reuse hub dan storage dari dashboard app — tidak buat instance baru
from apps.dashboard.app import app as _dashboard_app
from apps.dashboard.hub import hub, storage, registry
from apps.reconstruct.notifier import notify_window, notify_event

from apps.reconstruct.processor  import process_window
from apps.reconstruct.listener   import run as run_listener

logger = logging.getLogger(__name__)

# ── Shared state ──────────────────────────────────────────────────────────────

_nodes:      dict = {}
_nodes_lock: threading.Lock = threading.Lock()
_validator   = ValidatorRegistry()
_assessor    = QualityAssessor(phi=PHI)
_MODEL_DIR   = Path(__file__).resolve().parent / "ml_inference" / "models"


# ── MQTT worker thread ────────────────────────────────────────────────────────

def _run_mqtt_thread() -> None:
    """Blocking MQTT loop — dijalankan di background thread."""
    logger.info("MQTT worker thread started | broker=%s:%d", MQTT_BROKER, MQTT_PORT)
    try:
        run_listener(
            nodes        = _nodes,
            broker       = MQTT_BROKER,
            port         = MQTT_PORT,
            keepalive    = MQTT_KEEPALIVE,
            topic_base   = TOPIC_BASE,
            storage      = storage,
            processor_fn = process_window,
            validator    = _validator,
            assessor     = _assessor,
        )
    except Exception as exc:
        logger.critical("MQTT worker crashed: %s", exc, exc_info=True)


# ── Lifespan override ─────────────────────────────────────────────────────────

@asynccontextmanager
async def _combined_lifespan(app: FastAPI):
    """
    Ganti lifespan dashboard_server dengan versi yang juga start MQTT thread.
    """
    # Startup
    storage.open()
    hub.set_loop(asyncio.get_running_loop())
    registry.scan(_MODEL_DIR, recursive=True)

    mqtt_thread = threading.Thread(
        target  = _run_mqtt_thread,
        name    = "mqtt-worker",
        daemon  = True,   # mati otomatis saat main process exit
    )
    mqtt_thread.start()

    print("=" * 60)
    print("  ESP32 Health Monitor — All-in-One Server")
    print(f"  N={CS_N} M={CS_M} ({CS_M*100//CS_N}%)")
    print(f"  MQTT   : {MQTT_BROKER}:{MQTT_PORT}")
    print(f"  DB     : {DB_PATH} (retention={RETENTION_HOURS}h)")
    print(f"  API    : http://0.0.0.0:8000/docs")
    print(f"  WS     : ws://0.0.0.0:8000/ws/stream")
    print("=" * 60)

    yield

    # Shutdown
    storage.close()
    logger.info("Storage ditutup.")


# ── Buat app dengan lifespan override ────────────────────────────────────────

# Salin routes dari dashboard app, ganti lifespan saja
app = FastAPI(
    title       = _dashboard_app.title,
    description = _dashboard_app.description,
    version     = _dashboard_app.version,
    lifespan    = _combined_lifespan,
)

# Copy semua middleware dari dashboard app
for middleware in _dashboard_app.user_middleware:
    app.add_middleware(middleware.cls, **middleware.kwargs)

# Copy semua routes dari dashboard app
for route in _dashboard_app.routes:
    app.routes.append(route)


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    """Jalankan semua sistem server dalam satu proses."""
    setup_logging()
    uvicorn.run(
        app,
        host    = "0.0.0.0",
        port    = 8000,
        reload  = False,
        workers = 1,   # WAJIB 1 — hub & storage singleton tidak bisa di-fork
        log_config = None,  # biar setup_logging() yang handle format
    )


if __name__ == "__main__":
    main()
