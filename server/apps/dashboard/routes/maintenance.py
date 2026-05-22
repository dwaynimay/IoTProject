# File: server/apps/dashboard/routes/maintenance.py

from fastapi import APIRouter, Query

from core.config import RETENTION_HOURS
from apps.dashboard.hub import storage

router = APIRouter(tags=["Maintenance"])


@router.post(
    "/api/purge",
    summary="Hapus data lama (semua node)",
)
async def purge_old(
    max_age_hours: int = Query(
        RETENTION_HOURS,
        description="Hapus data lebih tua dari N jam",
        ge=1,
    ),
):
    """
    Hapus baris windows dan events yang lebih lama dari `max_age_hours`.
    Default menggunakan nilai retention dari config.
    """
    deleted = storage.purge_old(max_age_hours=max_age_hours)
    return {"deleted_rows": deleted, "max_age_hours": max_age_hours}


@router.delete(
    "/api/nodes/{node_id}/data",
    summary="Hapus semua data satu node",
)
async def delete_node_data(node_id: int):
    """
    Hapus semua windows dan events untuk node tertentu.
    Berguna untuk reset node saat debugging atau ganti hardware.
    """
    with storage._lock:
        dw = storage._conn.execute(
            "DELETE FROM windows WHERE node_id=?", (node_id,)
        ).rowcount
        de = storage._conn.execute(
            "DELETE FROM events WHERE node_id=?", (node_id,)
        ).rowcount
        storage._conn.commit()
    return {"node_id": node_id, "deleted_windows": dw, "deleted_events": de}
