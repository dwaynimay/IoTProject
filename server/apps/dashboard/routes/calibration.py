# File: server/apps/dashboard/routes/calibration.py

import os
from fastapi import APIRouter
from fastapi.responses import FileResponse

router = APIRouter(tags=["Tools"])

_STATIC_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', '..', 'static')
)

@router.get(
    "/calibration",
    summary="Halaman kalibrasi IMU",
    include_in_schema=False,
)
async def serve_calibration():
    """Sajikan halaman kalibrasi IMU dengan visualisasi 3D."""
    index = os.path.join(_STATIC_DIR, 'calibration.html')
    return FileResponse(index)
