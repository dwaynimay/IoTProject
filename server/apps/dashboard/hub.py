# File: server/apps/dashboard/hub.py

import asyncio
import time
from typing import Optional
from fastapi import WebSocket

from core.storage import StorageManager
from core.config import DB_PATH, RETENTION_HOURS


class BroadcastHub:
    """
    Thread-safe hub untuk push pesan ke semua WebSocket client.
    """

    def __init__(self) -> None:
        self._stream_clients: list[WebSocket] = []
        self._event_clients:  list[WebSocket] = []
        self._lock            = asyncio.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        """Simpan referensi event loop."""
        self._loop = loop

    async def connect_stream(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._stream_clients.append(ws)

    async def connect_events(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._event_clients.append(ws)

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self._stream_clients = [c for c in self._stream_clients if c is not ws]
            self._event_clients  = [c for c in self._event_clients  if c is not ws]

    async def _broadcast(self, clients: list[WebSocket], data: dict) -> None:
        """Kirim data ke semua client, hapus yang sudah disconnect."""
        dead: list[WebSocket] = []
        for ws in list(clients):
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        if dead:
            async with self._lock:
                self._stream_clients = [c for c in self._stream_clients if c not in dead]
                self._event_clients  = [c for c in self._event_clients  if c not in dead]

    async def publish_window(self, data: dict) -> None:
        await self._broadcast(self._stream_clients, data)

    async def publish_event(self, data: dict) -> None:
        await self._broadcast(self._event_clients, data)

    def publish_window_threadsafe(self, data: dict) -> None:
        """Dipanggil dari MQTT thread."""
        if self._loop and self._loop.is_running():
            asyncio.run_coroutine_threadsafe(self.publish_window(data), self._loop)

    def publish_event_threadsafe(self, data: dict) -> None:
        """Dipanggil dari MQTT thread."""
        if self._loop and self._loop.is_running():
            asyncio.run_coroutine_threadsafe(self.publish_event(data), self._loop)

    @property
    def stream_count(self) -> int:
        return len(self._stream_clients)

    @property
    def event_count(self) -> int:
        return len(self._event_clients)


# Module-level singletons
hub = BroadcastHub()
storage = StorageManager(db_path=DB_PATH, retention_hours=RETENTION_HOURS)

server_stats: dict = {
    "start_time_ms":    int(time.time() * 1000),
    "total_windows":    0,
    "total_rekon_ms":   0.0,
    "total_val_errors": 0,
    "total_low_quality": 0,
    "total_critical":    0,
}
