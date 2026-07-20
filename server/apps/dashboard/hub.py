# File: server/apps/dashboard/hub.py

import asyncio
import logging
import threading
import time
from typing import Optional
from fastapi import WebSocket

from core.storage import StorageManager
from core.config import DB_PATH, RETENTION_HOURS
from apps.ml_inference import ModelRegistry

logger = logging.getLogger(__name__)


class BroadcastHub:
    """
    Thread-safe hub untuk push pesan ke semua WebSocket client.
    """

    def __init__(self, send_timeout_s: float = 1.0) -> None:
        self._stream_clients: list[WebSocket] = []
        self._event_clients:  list[WebSocket] = []
        self._send_locks: dict[WebSocket, asyncio.Lock] = {}
        self._lock            = asyncio.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._send_timeout_s  = send_timeout_s

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        """Simpan referensi event loop."""
        self._loop = loop

    async def connect_stream(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._send_locks[ws] = asyncio.Lock()
            self._stream_clients.append(ws)

    async def connect_events(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._send_locks[ws] = asyncio.Lock()
            self._event_clients.append(ws)

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self._stream_clients = [c for c in self._stream_clients if c is not ws]
            self._event_clients  = [c for c in self._event_clients  if c is not ws]
            self._send_locks.pop(ws, None)

    async def send(self, ws: WebSocket, data: dict) -> bool:
        """Serialize writes per client and bound the time spent on slow sockets."""
        lock = self._send_locks.get(ws)
        if lock is None:
            return False
        try:
            async with asyncio.timeout(self._send_timeout_s):
                async with lock:
                    await ws.send_json(data)
            return True
        except Exception:
            return False

    async def _broadcast(self, clients: list[WebSocket], data: dict) -> None:
        """Kirim data ke semua client, hapus yang sudah disconnect."""
        snapshot = list(clients)
        results = await asyncio.gather(
            *(self.send(ws, data) for ws in snapshot),
            return_exceptions=False,
        )
        dead = [ws for ws, sent in zip(snapshot, results) if not sent]
        if dead:
            async with self._lock:
                dead_ids = {id(ws) for ws in dead}
                self._stream_clients = [c for c in self._stream_clients if id(c) not in dead_ids]
                self._event_clients  = [c for c in self._event_clients  if id(c) not in dead_ids]
                for ws in dead:
                    self._send_locks.pop(ws, None)

    async def publish_window(self, data: dict) -> None:
        await self._broadcast(self._stream_clients, data)

    async def publish_event(self, data: dict) -> None:
        await self._broadcast(self._event_clients, data)

    def publish_window_threadsafe(self, data: dict) -> None:
        """Dipanggil dari MQTT thread."""
        if self._loop and self._loop.is_running():
            future = asyncio.run_coroutine_threadsafe(self.publish_window(data), self._loop)
            future.add_done_callback(self._consume_publish_result)

    def publish_event_threadsafe(self, data: dict) -> None:
        """Dipanggil dari MQTT thread."""
        if self._loop and self._loop.is_running():
            future = asyncio.run_coroutine_threadsafe(self.publish_event(data), self._loop)
            future.add_done_callback(self._consume_publish_result)

    @staticmethod
    def _consume_publish_result(future) -> None:
        try:
            future.result()
        except Exception:
            logger.exception("WebSocket publish gagal")

    @property
    def stream_count(self) -> int:
        return len(self._stream_clients)

    @property
    def event_count(self) -> int:
        return len(self._event_clients)


# Module-level singletons
hub = BroadcastHub()
storage = StorageManager(db_path=DB_PATH, retention_hours=RETENTION_HOURS)
registry = ModelRegistry()

server_stats: dict = {
    "start_time_ms":    int(time.time() * 1000),
    "total_windows":    0,
    "total_rekon_ms":   0.0,
    "total_val_errors": 0,
    "total_low_quality": 0,
    "total_critical":    0,
}
server_stats_lock = threading.Lock()
