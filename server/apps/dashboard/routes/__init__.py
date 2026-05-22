# File: server/apps/dashboard/routes/__init__.py

from fastapi import APIRouter

from .status import router as status_router
from .nodes import router as nodes_router
from .windows import router as windows_router
from .events import router as events_router
from .metrics import router as metrics_router
from .maintenance import router as maintenance_router

router = APIRouter()

router.include_router(status_router)
router.include_router(nodes_router)
router.include_router(windows_router)
router.include_router(events_router)
router.include_router(metrics_router)
router.include_router(maintenance_router)
