# File: server/apps/dashboard/routes/windows.py

from fastapi import APIRouter, HTTPException, Query

from core.config import SIGNALS
from apps.dashboard.hub import storage

router = APIRouter(tags=["Node"])


@router.get(
    "/api/nodes/{node_id}/windows",
    summary="N window terakhir untuk satu sinyal",
)
def get_windows(
    node_id: int,
    signal:  str  = Query("ax",  description="Sinyal: ax/ay/az/gx/gy/gz/ir"),
    n:       int  = Query(20,    description="Jumlah window", ge=1, le=500),
    include_values: bool = Query(False, description="Sertakan array nilai rekonstruksi (64 float)"),
):
    """
    Ambil N window terakhir untuk satu sinyal dari satu node.
    Default tanpa array nilai untuk menghemat bandwidth.
    Aktifkan `include_values=true` jika ingin plot di client.
    """
    valid_signals = SIGNALS
    if signal not in valid_signals:
        raise HTTPException(
            status_code=400,
            detail=f"Signal '{signal}' tidak valid. Pilihan: {valid_signals}",
        )

    rows = storage.get_last_windows(node_id=node_id, signal=signal, n=n)

    if not include_values:
        for r in rows:
            r.pop("values", None)

    return {
        "node_id": node_id,
        "signal":  signal,
        "count":   len(rows),
        "windows": rows,
    }
