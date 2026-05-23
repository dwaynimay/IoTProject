from fastapi import APIRouter
from apps.dashboard.hub import registry

router = APIRouter()

@router.get("/api/ml/status")
async def get_ml_status():
    """
    Mengembalikan status ModelRegistry.
    Jika folder models/ kosong, kembalikan response default yang aman.
    """
    return registry.status()
