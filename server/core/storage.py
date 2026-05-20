# File: server/core/storage.py

# =============================================================================
# storage.py — Time-Series Storage (F6)
# =============================================================================
#
# SQLite ringan — tidak butuh install server eksternal.
# Dua tabel utama:
#
#   windows : satu baris per sinyal per window rekonstruksi
#   ┌──────────────┬──────────────────────────────────────────────────┐
#   │ id           │ INTEGER PRIMARY KEY AUTOINCREMENT                │
#   │ node_id      │ INTEGER  — 1, 2, dst                            │
#   │ window_num   │ INTEGER  — nomor window per node                 │
#   │ ts_sensor_ms │ INTEGER  — timestamp dari firmware (millis)      │
#   │ ts_server_ms │ INTEGER  — waktu server terima (epoch ms)        │
#   │ signal       │ TEXT     — "ax", "gz", "ir", dst                 │
#   │ values_json  │ TEXT     — JSON array float64 panjang CS_N       │
#   │ rel_error    │ REAL     — relative reconstruction error         │
#   │ sparsity     │ REAL     — sparsity ratio koefisien DCT          │
#   │ snr_db       │ REAL     — estimasi SNR dB                       │
#   │ quality_flag │ TEXT     — "OK" / "LOW_QUALITY" / "CRITICAL"     │
#   │ hr           │ INTEGER  — heart rate dari PPG (-1 jika N/A)     │
#   │ spo2         │ REAL     — SpO2 % (0.0 jika tidak valid)        │
#   │ finger       │ INTEGER  — 1 jika jari terdeteksi                │
#   └──────────────┴──────────────────────────────────────────────────┘
#
#   events : log kejadian anomali / info
#   ┌──────────────┬──────────────────────────────────────────────────┐
#   │ id           │ INTEGER PRIMARY KEY AUTOINCREMENT                │
#   │ node_id      │ INTEGER                                          │
#   │ ts_server_ms │ INTEGER                                          │
#   │ event_type   │ TEXT  — "LOW_QUALITY", "CRITICAL", "INVALID",   │
#   │              │          "NODE_REGISTERED", dst                  │
#   │ detail       │ TEXT  — pesan deskriptif                         │
#   └──────────────┴──────────────────────────────────────────────────┘
#
# CARA PAKAI di reconstruct_server.py:
#
#   from core.storage import StorageManager
#
#   db = StorageManager()          # buka/buat DB di path default
#   db.open()                      # panggil sekali di awal
#
#   # setelah assess_window():
#   db.save_window(
#       node_id    = 1,
#       window_num = 5,
#       ts_sensor  = 12345,
#       results    = {"ax": x_hat_ax, ...},
#       report     = quality_report,   # WindowReport dari quality.py
#       hr         = 75,
#       spo2       = 98.2,
#       finger     = True,
#   )
#
#   # log event:
#   db.log_event(node_id=1, event_type="LOW_QUALITY", detail="gx gz")
#
#   # query:
#   rows = db.get_last_windows(node_id=1, signal="ax", n=10)
#   stats = db.get_node_stats(node_id=1)
#
#   # maintenance (jalankan periodik atau saat startup):
#   deleted = db.purge_old(max_age_hours=24)
#
# THREAD SAFETY:
#   SQLite dengan check_same_thread=False + WAL mode aman untuk
#   multi-thread read. Write di-serialisasi via threading.Lock internal.
#   Jika ada concern, bungkus save_window() dengan Lock di level app.
#
# RETENTION:
#   Default 24 jam. Ubah via StorageManager(retention_hours=48).
#   purge_old() harus dipanggil manual / periodik dari task atau timer.
# =============================================================================

from __future__ import annotations

import json
import sqlite3
import threading
import time
from pathlib import Path
from typing import Optional

import numpy as np

try:
    from .quality import WindowReport, QualityFlag
    from .config  import SIGNALS
except ImportError:
    # Fallback untuk standalone / unit test
    WindowReport = None   # type: ignore
    QualityFlag  = None   # type: ignore
    SIGNALS      = ["ax", "ay", "az", "gx", "gy", "gz", "ir"]


# =============================================================================
# Konfigurasi default
# =============================================================================

DEFAULT_DB_PATH      = Path("health_monitor.db")
DEFAULT_RETENTION_H  = 24    # jam


# =============================================================================
# StorageManager
# =============================================================================

class StorageManager:
    """
    Kelola koneksi SQLite dan semua operasi baca-tulis.

    Args:
        db_path         : path file SQLite (default: "health_monitor.db")
        retention_hours : data lebih tua dari ini akan dihapus saat purge_old()
    """

    def __init__(
        self,
        db_path:         "str | Path" = DEFAULT_DB_PATH,
        retention_hours: int          = DEFAULT_RETENTION_H,
    ) -> None:
        self._path           = Path(db_path)
        self._retention_hours = retention_hours
        self._conn: Optional[sqlite3.Connection] = None
        self._lock           = threading.Lock()

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def open(self) -> None:
        """
        Buka koneksi ke database dan buat tabel jika belum ada.
        Panggil sekali di awal sebelum operasi apapun.
        """
        self._conn = sqlite3.connect(
            str(self._path),
            check_same_thread = False,  # kita handle locking sendiri
        )
        self._conn.execute("PRAGMA journal_mode=WAL")   # concurrent read
        self._conn.execute("PRAGMA synchronous=NORMAL") # balance speed/safety
        self._create_tables()
        print(f"[Storage] DB: {self._path.resolve()} | retention={self._retention_hours}h")

    def close(self) -> None:
        """Tutup koneksi. Panggil saat shutdown server."""
        if self._conn:
            self._conn.close()
            self._conn = None

    def __enter__(self) -> "StorageManager":
        self.open()
        return self

    def __exit__(self, *_) -> None:
        self.close()

    # ── Write API ─────────────────────────────────────────────────────────────

    def save_window(
        self,
        node_id:    int,
        window_num: int,
        ts_sensor:  int,
        results:    "dict[str, np.ndarray]",
        report:     "WindowReport | None" = None,
        hr:         int   = -1,
        spo2:       float = 0.0,
        finger:     bool  = False,
    ) -> int:
        """
        Simpan satu window rekonstruksi ke tabel `windows`.
        Satu window → beberapa baris (satu per sinyal).

        Args:
            node_id    : ID node
            window_num : nomor window (dari NodeState.windows_done)
            ts_sensor  : timestamp dari firmware (ms)
            results    : dict sinyal → ndarray rekonstruksi (panjang CS_N)
            report     : WindowReport dari quality.py (opsional, boleh None)
            hr         : heart rate BPM (-1 jika tidak ada)
            spo2       : SpO2 % (0.0 jika tidak valid)
            finger     : True jika jari terdeteksi

        Returns:
            Jumlah baris yang berhasil diinsert.
        """
        self._ensure_open()

        ts_server = _now_ms()
        rows      = []

        for sig, x_hat in results.items():
            # Ambil metrik dari report jika tersedia
            rel_error    = None
            sparsity     = None
            snr_db       = None
            quality_flag = None

            if report is not None and sig in report.metrics:
                m            = report.metrics[sig]
                rel_error    = float(m.relative_error)
                sparsity     = float(m.sparsity_ratio)
                snr_db       = float(m.snr_db) if m.snr_db != float("inf") else 999.0
                quality_flag = m.flag.value

            rows.append((
                node_id,
                window_num,
                ts_sensor,
                ts_server,
                sig,
                json.dumps(x_hat.tolist()),  # ndarray → JSON string
                rel_error,
                sparsity,
                snr_db,
                quality_flag,
                hr,
                spo2,
                int(finger),
            ))

        with self._lock:
            self._conn.executemany(
                """
                INSERT INTO windows
                    (node_id, window_num, ts_sensor_ms, ts_server_ms,
                     signal, values_json,
                     rel_error, sparsity, snr_db, quality_flag,
                     hr, spo2, finger)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                rows,
            )
            self._conn.commit()

        return len(rows)

    def log_event(
        self,
        node_id:    int,
        event_type: str,
        detail:     str = "",
    ) -> None:
        """
        Catat satu event ke tabel `events`.

        event_type contoh:
          "NODE_REGISTERED", "LOW_QUALITY", "CRITICAL",
          "VALIDATION_ERROR", "SENSOR_DROPPED"
        """
        self._ensure_open()

        with self._lock:
            self._conn.execute(
                """
                INSERT INTO events (node_id, ts_server_ms, event_type, detail)
                VALUES (?, ?, ?, ?)
                """,
                (_sanitize_node_id(node_id), _now_ms(), event_type[:64], detail[:512]),
            )
            self._conn.commit()

    # ── Read API ──────────────────────────────────────────────────────────────

    def get_last_windows(
        self,
        node_id: int,
        signal:  str,
        n:       int = 10,
    ) -> list[dict]:
        """
        Ambil N window terakhir untuk sinyal tertentu dari node tertentu.

        Returns:
            list of dict dengan key:
              window_num, ts_sensor_ms, ts_server_ms,
              values (list float), rel_error, sparsity, snr_db,
              quality_flag, hr, spo2, finger
        """
        self._ensure_open()

        rows = self._conn.execute(
            """
            SELECT window_num, ts_sensor_ms, ts_server_ms,
                   values_json, rel_error, sparsity, snr_db,
                   quality_flag, hr, spo2, finger
            FROM   windows
            WHERE  node_id = ? AND signal = ?
            ORDER  BY id DESC
            LIMIT  ?
            """,
            (node_id, signal, n),
        ).fetchall()

        return [_row_to_dict(r) for r in reversed(rows)]  # kronologis

    def get_last_events(
        self,
        node_id:    Optional[int] = None,
        event_type: Optional[str] = None,
        n:          int = 20,
    ) -> list[dict]:
        """
        Ambil N event terakhir, opsional filter node_id dan/atau event_type.
        """
        self._ensure_open()

        query  = "SELECT node_id, ts_server_ms, event_type, detail FROM events"
        params: list = []
        clauses: list[str] = []

        if node_id is not None:
            clauses.append("node_id = ?")
            params.append(node_id)
        if event_type is not None:
            clauses.append("event_type = ?")
            params.append(event_type)

        if clauses:
            query += " WHERE " + " AND ".join(clauses)
        query += " ORDER BY ts_server_ms DESC LIMIT ?"
        params.append(n)

        rows = self._conn.execute(query, params).fetchall()
        return [
            {
                "node_id":    r[0],
                "ts_ms":      r[1],
                "event_type": r[2],
                "detail":     r[3],
            }
            for r in rows
        ]

    def get_node_stats(self, node_id: int) -> dict:
        """
        Statistik ringkas untuk satu node (berguna untuk dashboard).

        Returns dict:
          total_windows, low_quality_count, critical_count,
          avg_rel_error, last_hr, last_spo2, last_seen_ms
        """
        self._ensure_open()

        row = self._conn.execute(
            """
            SELECT
                COUNT(DISTINCT window_num)                              AS total_windows,
                COUNT(CASE WHEN quality_flag='LOW_QUALITY' THEN 1 END) AS low_q,
                COUNT(CASE WHEN quality_flag='CRITICAL'    THEN 1 END) AS crit,
                AVG(rel_error)                                          AS avg_err,
                MAX(ts_server_ms)                                       AS last_seen
            FROM windows
            WHERE node_id = ?
            """,
            (node_id,),
        ).fetchone()

        # HR dan SpO2 dari window paling baru
        last = self._conn.execute(
            """
            SELECT hr, spo2
            FROM   windows
            WHERE  node_id = ? AND signal = 'ir'
            ORDER  BY id DESC
            LIMIT  1
            """,
            (node_id,),
        ).fetchone()

        return {
            "node_id":          node_id,
            "total_windows":    row[0] or 0,
            "low_quality_count": row[1] or 0,
            "critical_count":   row[2] or 0,
            "avg_rel_error":    round(row[3], 4) if row[3] else 0.0,
            "last_seen_ms":     row[4] or 0,
            "last_hr":          last[0] if last else -1,
            "last_spo2":        last[1] if last else 0.0,
        }

    def get_all_node_ids(self) -> list[int]:
        """Daftar semua node_id yang pernah kirim data."""
        self._ensure_open()
        rows = self._conn.execute(
            "SELECT DISTINCT node_id FROM windows ORDER BY node_id"
        ).fetchall()
        return [r[0] for r in rows]

    # ── Maintenance ───────────────────────────────────────────────────────────

    def purge_old(self, max_age_hours: Optional[int] = None) -> int:
        """
        Hapus data lebih tua dari max_age_hours.
        Default: gunakan retention_hours yang diset saat konstruksi.

        Returns:
            Total baris yang dihapus (windows + events)
        """
        self._ensure_open()

        age_h     = max_age_hours if max_age_hours is not None else self._retention_hours
        cutoff_ms = _now_ms() - age_h * 3_600_000

        with self._lock:
            cur_w = self._conn.execute(
                "DELETE FROM windows WHERE ts_server_ms < ?", (cutoff_ms,)
            )
            cur_e = self._conn.execute(
                "DELETE FROM events  WHERE ts_server_ms < ?", (cutoff_ms,)
            )
            self._conn.commit()

        deleted = cur_w.rowcount + cur_e.rowcount
        if deleted > 0:
            print(f"[Storage] Purge: {deleted} baris dihapus (> {age_h}h)")
        return deleted

    def db_size_bytes(self) -> int:
        """Ukuran file DB dalam byte."""
        return self._path.stat().st_size if self._path.exists() else 0

    # ── Internal ──────────────────────────────────────────────────────────────

    def _create_tables(self) -> None:
        self._conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS windows (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                node_id       INTEGER NOT NULL,
                window_num    INTEGER NOT NULL,
                ts_sensor_ms  INTEGER NOT NULL,
                ts_server_ms  INTEGER NOT NULL,
                signal        TEXT    NOT NULL,
                values_json   TEXT    NOT NULL,
                rel_error     REAL,
                sparsity      REAL,
                snr_db        REAL,
                quality_flag  TEXT,
                hr            INTEGER,
                spo2          REAL,
                finger        INTEGER
            );

            CREATE INDEX IF NOT EXISTS idx_windows_node_signal
                ON windows (node_id, signal, ts_server_ms DESC);

            CREATE INDEX IF NOT EXISTS idx_windows_ts
                ON windows (ts_server_ms);

            CREATE TABLE IF NOT EXISTS events (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                node_id       INTEGER NOT NULL,
                ts_server_ms  INTEGER NOT NULL,
                event_type    TEXT    NOT NULL,
                detail        TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_events_node
                ON events (node_id, ts_server_ms DESC);
            """
        )
        self._conn.commit()

    def _ensure_open(self) -> None:
        if self._conn is None:
            raise RuntimeError(
                "StorageManager belum dibuka. Panggil open() atau gunakan 'with' statement."
            )


# =============================================================================
# Helpers
# =============================================================================

def _now_ms() -> int:
    """Epoch time dalam milidetik."""
    return int(time.time() * 1000)


def _sanitize_node_id(node_id: int) -> int:
    return max(0, min(int(node_id), 9999))


def _row_to_dict(row: tuple) -> dict:
    """Konversi tuple result SELECT ke dict dengan values sudah di-decode."""
    return {
        "window_num":   row[0],
        "ts_sensor_ms": row[1],
        "ts_server_ms": row[2],
        "values":       json.loads(row[3]),
        "rel_error":    row[4],
        "sparsity":     row[5],
        "snr_db":       row[6],
        "quality_flag": row[7],
        "hr":           row[8],
        "spo2":         row[9],
        "finger":       bool(row[10]),
    }
