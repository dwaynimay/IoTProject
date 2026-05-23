"""
schemas.py — Kontrak data I/O untuk ML Inference Engine.

Semua modul harus pakai tipe ini — jangan dict mentah.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Optional


# ── Input ────────────────────────────────────────────────────────────────────

@dataclass
class WindowInput:
    """
    Data satu window rekonstruksi yang akan di-infer.

    Semua sinyal berupa list/array panjang CS_N (64).
    Field yang None berarti tidak tersedia (sensor off/finger not detected).
    """
    node_id:    int
    window_num: int
    ts:         int                         # timestamp firmware (ms)

    # IMU — rekonstruksi dari CS (panjang CS_N = 64)
    ax: Optional[list[float]] = None
    ay: Optional[list[float]] = None
    az: Optional[list[float]] = None
    gx: Optional[list[float]] = None
    gy: Optional[list[float]] = None
    gz: Optional[list[float]] = None

    # PPG — rekonstruksi IR dari CS
    ir: Optional[list[float]] = None

    # Metadata
    hr:     int            = -1
    spo2:   Optional[float] = None
    finger: bool           = False


# ── Output single model ───────────────────────────────────────────────────────

@dataclass
class InferenceResult:
    """
    Hasil inferensi satu model untuk satu window.

    label       : kelas prediksi terbaik — diambil dari labels di config,
                  jadi nama label sepenuhnya dikontrol dari JSON, bukan hardcode.
    confidence  : probabilitas kelas terbaik (0.0–1.0)
    proba       : dict label → probabilitas, urutan sesuai config labels
    feature_vec : vektor fitur yang dipakai (debug)
    skipped     : True jika window di-skip
    skip_reason : alasan skip
    model_name  : nama model yang menghasilkan ini (diisi oleh registry)
    elapsed_ms  : waktu inferensi dalam ms
    """
    node_id:    int
    window_num: int
    ts:         int

    label:       str              = "unknown"
    confidence:  float            = 0.0
    proba:       dict[str, float] = field(default_factory=dict)
    feature_vec: list[float]      = field(default_factory=list)

    skipped:     bool  = False
    skip_reason: str   = ""
    model_name:  str   = ""
    elapsed_ms:  float = 0.0

    def is_ok(self) -> bool:
        return not self.skipped

    def top_label(self) -> str:
        if not self.proba:
            return self.label
        return max(self.proba, key=self.proba.get)

    def top_n(self, n: int = 3) -> list[tuple[str, float]]:
        """N label teratas beserta probabilitasnya, urut descending."""
        return sorted(self.proba.items(), key=lambda x: x[1], reverse=True)[:n]

    def to_dict(self) -> dict:
        """Serialisasi ke dict untuk WebSocket / REST."""
        return {
            "model_name":  self.model_name,
            "label":       self.label,
            "confidence":  round(self.confidence, 4),
            "proba":       {k: round(v, 4) for k, v in self.proba.items()},
            "skipped":     self.skipped,
            "skip_reason": self.skip_reason,
            "elapsed_ms":  round(self.elapsed_ms, 3),
        }

    def short_str(self) -> str:
        if self.skipped:
            return f"[SKIP:{self.model_name}] win={self.window_num} reason={self.skip_reason}"
        top3_str = " ".join(f"{k}:{v:.3f}" for k, v in self.top_n(3))
        return (
            f"[{self.model_name}] node={self.node_id} win={self.window_num} "
            f"→ {self.label} ({self.confidence:.3f}) | {top3_str}"
        )


# ── Output multi-model (parallel) ────────────────────────────────────────────

@dataclass
class MultiModelResult:
    """
    Agregasi hasil semua model yang jalan parallel untuk satu window.

    Desain dinamis: tidak ada label yang hardcode di sini.
    Setiap model membawa label-nya sendiri dari config masing-masing.
    Dashboard cukup iterasi results.items() untuk tampilkan semua prediksi,
    apapun label yang ada di sana.

    results     : dict model_name → InferenceResult
    ts_inferred : epoch ms saat inferensi selesai (untuk dashboard)
    total_ms    : wall time seluruh inferensi parallel (bukan sum)

    Cara pakai:
        mmr = MultiModelResult(node_id=1, window_num=5, ts=12000)
        mmr.add(result_activity)   # labels: Sitting, Walking, Lying Down, Fall
        mmr.add(result_stress)     # labels: Low, Medium, High  ← tambah kapanpun

        mmr["activity_classifier"]   # InferenceResult model tertentu
        mmr.labels()                 # {"activity_classifier": "Walking", "stress": "Low"}
        mmr.to_dict()                # siap kirim ke dashboard via WS
    """
    node_id:    int
    window_num: int
    ts:         int

    results:     dict[str, InferenceResult] = field(default_factory=dict)
    ts_inferred: int   = field(default_factory=lambda: int(time.time() * 1000))
    total_ms:    float = 0.0

    def add(self, result: InferenceResult) -> None:
        """Tambah hasil satu model. model_name diambil dari result.model_name."""
        self.results[result.model_name] = result

    def __getitem__(self, model_name: str) -> InferenceResult:
        return self.results[model_name]

    def __contains__(self, model_name: str) -> bool:
        return model_name in self.results

    def model_names(self) -> list[str]:
        return list(self.results.keys())

    def labels(self) -> dict[str, str]:
        """dict model_name → label terprediksi. Hanya model yang tidak skip."""
        return {
            name: r.label
            for name, r in self.results.items()
            if not r.skipped
        }

    def confidences(self) -> dict[str, float]:
        """dict model_name → confidence top label."""
        return {
            name: r.confidence
            for name, r in self.results.items()
            if not r.skipped
        }

    def active_count(self) -> int:
        """Jumlah model yang tidak skip."""
        return sum(1 for r in self.results.values() if not r.skipped)

    def skipped_count(self) -> int:
        return sum(1 for r in self.results.values() if r.skipped)

    def to_dict(self) -> dict:
        """
        Serialisasi ke dict untuk WebSocket / REST API dashboard.

        Format output sepenuhnya mengikuti label yang ada di tiap model —
        dashboard tidak perlu tahu label apa yang mungkin muncul sebelumnya.

        {
            "node_id": 1,
            "window_num": 5,
            "ts": 12000,
            "ts_inferred": 1717000000000,
            "total_ms": 4.2,
            "models": {
                "activity_classifier": {
                    "label": "Walking",
                    "confidence": 0.87,
                    "proba": {"Sitting": 0.05, "Walking": 0.87, ...},
                    ...
                },
                "fall_detector": { ... },
                "stress_monitor": { ... }   ← model baru otomatis muncul di sini
            }
        }
        """
        return {
            "node_id":     self.node_id,
            "window_num":  self.window_num,
            "ts":          self.ts,
            "ts_inferred": self.ts_inferred,
            "total_ms":    round(self.total_ms, 2),
            "models":      {name: r.to_dict() for name, r in self.results.items()},
        }

    def summary_str(self) -> str:
        """Satu baris ringkasan semua model — berguna untuk logging."""
        parts = []
        for name, r in self.results.items():
            if r.skipped:
                parts.append(f"{name}:SKIP")
            else:
                parts.append(f"{name}:{r.label}({r.confidence:.2f})")
        return f"win={self.window_num} | " + " | ".join(parts)
