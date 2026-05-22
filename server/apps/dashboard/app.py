# File: server/apps/dashboard/app.py

import os
import asyncio
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, JSONResponse

from core.config import DB_PATH
from apps.dashboard.hub import hub, storage
from apps.dashboard.routes import router as api_router
from apps.dashboard import websocket


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    storage.open()
    hub.set_loop(asyncio.get_running_loop())
    print(f"[Dashboard] Server siap | DB: {DB_PATH}")
    print(f"[Dashboard] Docs: http://127.0.0.1:8000/docs")
    yield
    # Shutdown
    storage.close()
    print("[Dashboard] Storage ditutup.")


app = FastAPI(
    title       = "Health Monitor Dashboard API",
    description = (
        "REST + WebSocket API untuk ESP32 Health Monitor.\n\n"
        "**Jalankan bersamaan dengan** `reconstruct`."
    ),
    version     = "1.0.0",
    lifespan    = lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins     = ["*"],
    allow_credentials = True,
    allow_methods     = ["*"],
    allow_headers     = ["*"],
)

# Register routes
app.include_router(api_router)

# Register WebSockets
app.add_api_websocket_route("/ws/stream", websocket.ws_stream)
app.add_api_websocket_route("/ws/events", websocket.ws_events)

# Serve static dashboard
_STATIC_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'static'))
if os.path.isdir(_STATIC_DIR):
    app.mount("/static", StaticFiles(directory=_STATIC_DIR), name="static")


@app.get("/", include_in_schema=False)
async def serve_dashboard():
    index = os.path.join(_STATIC_DIR, 'index.html')
    if os.path.exists(index):
        return FileResponse(index)
    return JSONResponse({"message": "Dashboard tidak ditemukan. Letakkan index.html di server/static/"})
