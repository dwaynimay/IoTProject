# File: server/apps/dashboard/routes/maintenance.py

import secrets
from typing import Annotated, Optional

from fastapi import APIRouter, Header, HTTPException, Query

from core.config import ADMIN_API_TOKEN, RETENTION_HOURS
from apps.dashboard.hub import storage

router = APIRouter(tags=["Maintenance"])


def require_admin(
    x_admin_token: Annotated[Optional[str], Header(alias="X-Admin-Token")] = None,
) -> None:
    if not ADMIN_API_TOKEN:
        raise HTTPException(
            status_code=503,
            detail="Maintenance API nonaktif; set ADMIN_API_TOKEN untuk mengaktifkan",
        )
    if x_admin_token is None or not secrets.compare_digest(x_admin_token, ADMIN_API_TOKEN):
        raise HTTPException(status_code=401, detail="Admin token tidak valid")


@router.post(
    "/api/purge",
    summary="Hapus data lama (semua node)",
)
def purge_old(
    max_age_hours: int = Query(
        RETENTION_HOURS,
        description="Hapus data lebih tua dari N jam",
        ge=1,
    ),
    x_admin_token: Annotated[Optional[str], Header(alias="X-Admin-Token")] = None,
):
    """
    Hapus baris windows dan events yang lebih lama dari `max_age_hours`.
    Default menggunakan nilai retention dari config.
    """
    require_admin(x_admin_token)
    deleted = storage.purge_old(max_age_hours=max_age_hours)
    return {"deleted_rows": deleted, "max_age_hours": max_age_hours}


@router.delete(
    "/api/nodes/{node_id}/data",
    summary="Hapus semua data satu node",
)
def delete_node_data(
    node_id: int,
    x_admin_token: Annotated[Optional[str], Header(alias="X-Admin-Token")] = None,
):
    """
    Hapus semua windows dan events untuk node tertentu.
    Berguna untuk reset node saat debugging atau ganti hardware.
    """
    require_admin(x_admin_token)
    with storage._lock:
        dw = storage._conn.execute(
            "DELETE FROM windows WHERE node_id=?", (node_id,)
        ).rowcount
        de = storage._conn.execute(
            "DELETE FROM events WHERE node_id=?", (node_id,)
        ).rowcount
        storage._conn.commit()
    return {"node_id": node_id, "deleted_windows": dw, "deleted_events": de}
