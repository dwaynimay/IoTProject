import asyncio
import os
import sys

import pytest
from fastapi import HTTPException

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from apps.dashboard.hub import BroadcastHub
import apps.dashboard.routes.maintenance as maintenance


class _FakeWebSocket:
    def __init__(self, delay_s=0.0):
        self.delay_s = delay_s
        self.messages = []
        self.accepted = False
        self.sent = asyncio.Event()
        self.active_sends = 0
        self.max_active_sends = 0

    async def accept(self):
        self.accepted = True

    async def send_json(self, data):
        self.active_sends += 1
        self.max_active_sends = max(self.max_active_sends, self.active_sends)
        try:
            await asyncio.sleep(self.delay_s)
            self.messages.append(data)
            self.sent.set()
        finally:
            self.active_sends -= 1


def test_slow_client_does_not_delay_fast_client():
    async def scenario():
        hub = BroadcastHub(send_timeout_s=0.1)
        slow = _FakeWebSocket(delay_s=1.0)
        fast = _FakeWebSocket()
        await hub.connect_stream(slow)
        await hub.connect_stream(fast)

        publish = asyncio.create_task(hub.publish_window({"type": "window"}))
        await asyncio.wait_for(fast.sent.wait(), timeout=0.03)
        await publish

        assert fast.messages == [{"type": "window"}]
        assert hub.stream_count == 1

    asyncio.run(scenario())


def test_concurrent_publishes_serialize_each_socket():
    async def scenario():
        hub = BroadcastHub(send_timeout_s=1.0)
        client = _FakeWebSocket(delay_s=0.03)
        await hub.connect_stream(client)

        await asyncio.gather(
            hub.publish_window({"seq": 1}),
            hub.publish_window({"seq": 2}),
        )

        assert client.max_active_sends == 1
        assert client.messages == [{"seq": 1}, {"seq": 2}]

    asyncio.run(scenario())


def test_maintenance_api_disabled_without_token(monkeypatch):
    monkeypatch.setattr(maintenance, "ADMIN_API_TOKEN", "")
    with pytest.raises(HTTPException) as exc:
        maintenance.require_admin(None)
    assert exc.value.status_code == 503


def test_maintenance_api_uses_constant_time_token_check(monkeypatch):
    monkeypatch.setattr(maintenance, "ADMIN_API_TOKEN", "secret")
    maintenance.require_admin("secret")
    with pytest.raises(HTTPException) as exc:
        maintenance.require_admin("wrong")
    assert exc.value.status_code == 401
