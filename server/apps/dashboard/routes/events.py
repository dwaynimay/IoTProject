# File: server/apps/dashboard/routes/events.py

from typing import Optional
from fastapi import APIRouter, HTTPException, Query

from apps.dashboard.hub import storage

router = APIRouter()


def _node_or_404(node_id: int) -> None:
    if node_id not in storage.get_all_node_ids():
        raise HTTPException(status_code=404, detail=f"Node {node_id} tidak ditemukan")


@router.get(
    "/api/nodes/{node_id}/events",
    summary="Event log satu node",
    tags=["Node"],
)
async def get_node_events(
    node_id:    int,
    event_type: Optional[str] = Query(
        None,
        description="Filter tipe: LOW_QUALITY / CRITICAL / VALIDATION_ERROR / NODE_REGISTERED",
    ),
    n: int = Query(20, ge=1, le=500),
):
    """Event log untuk satu node, opsional filter per tipe."""
    _node_or_404(node_id)
    events = storage.get_last_events(node_id=node_id, event_type=event_type, n=n)
    return {"node_id": node_id, "count": len(events), "events": events}


@router.get(
    "/api/events",
    summary="Semua event terbaru lintas node",
    tags=["Overview"],
)
async def get_all_events(
    event_type: Optional[str] = Query(None, description="Filter tipe event"),
    n:          int            = Query(50, ge=1, le=500),
):
    """Semua event terbaru dari semua node, opsional filter per tipe."""
    events = storage.get_last_events(event_type=event_type, n=n)
    return {"count": len(events), "events": events}
