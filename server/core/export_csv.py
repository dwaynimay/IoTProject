# =============================================================================
# export_csv.py — Export Engine: IMU + PPG dari health_monitor.db → CSV
# =============================================================================
#
# CARA PAKAI:
#
#   # Export semua node, semua data:
#   python export_csv.py
#
#   # Export node tertentu:
#   python export_csv.py --node 1
#
#   # Filter rentang waktu (epoch ms):
#   python export_csv.py --node 1 --from-ms 1700000000000 --to-ms 1700086400000
#
#   # Pilih path DB dan output:
#   python export_csv.py --db data/health_monitor.db --out hasil_export.csv
#
#   # Hanya sinyal PPG (ir) + HR/SpO2:
#   python export_csv.py --signals ir
#
#   # Hanya sinyal IMU:
#   python export_csv.py --signals ax ay az gx gy gz
#
# FORMAT OUTPUT CSV:
#   node_id, window_num, ts_sensor_ms, ts_server_ms, signal,
#   hr, spo2, finger, quality_flag, rel_error, sparsity, snr_db,
#   val_0, val_1, ..., val_N   (kolom nilai rekonstruksi)
# =============================================================================

from __future__ import annotations

import argparse
import csv
import json
import logging
import sqlite3
import sys
from pathlib import Path
from typing import Iterator, Optional

logging.basicConfig(
    level   = logging.INFO,
    format  = "%(asctime)s [%(levelname)s] %(message)s",
    datefmt = "%H:%M:%S",
)
logger = logging.getLogger(__name__)

# Sinyal default yang akan di-export
IMU_SIGNALS = ["ax", "ay", "az", "gx", "gy", "gz"]
PPG_SIGNALS = ["ir"]
ALL_SIGNALS = IMU_SIGNALS + PPG_SIGNALS


# =============================================================================
# Core Export Engine
# =============================================================================

class CSVExporter:
    """
    Export data IMU + PPG dari tabel `windows` ke file CSV.

    Args:
        db_path  : path ke file SQLite
        out_path : path output CSV (default: health_export.csv)
        signals  : list sinyal yang di-export (default: semua)
        node_id  : filter node tertentu (None = semua node)
        from_ms  : filter ts_server_ms >= from_ms
        to_ms    : filter ts_server_ms <= to_ms
        chunk    : jumlah baris per batch query (efisiensi memori)
    """

    def __init__(
        self,
        db_path:  "str | Path"   = "data/health_monitor.db",
        out_path: "str | Path"   = "health_export.csv",
        signals:  list[str]      = None,
        node_id:  Optional[int]  = None,
        from_ms:  Optional[int]  = None,
        to_ms:    Optional[int]  = None,
        chunk:    int            = 500,
        fmt:      str            = "wide",
    ) -> None:
        self.db_path  = Path(db_path)
        self.out_path = Path(out_path)
        self.signals  = signals or ALL_SIGNALS
        self.node_id  = node_id
        self.from_ms  = from_ms
        self.to_ms    = to_ms
        self.chunk    = chunk
        self.fmt      = fmt

    # ── Public ────────────────────────────────────────────────────────────────

    def run(self) -> int:
        """
        Jalankan export. Kembalikan jumlah baris yang ditulis ke CSV.
        """
        if not self.db_path.exists():
            logger.error("DB tidak ditemukan: %s", self.db_path.resolve())
            sys.exit(1)

        logger.info("Membuka DB: %s", self.db_path.resolve())
        conn = sqlite3.connect(str(self.db_path), check_same_thread=False)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.row_factory = sqlite3.Row

        try:
            if self.fmt == "stacked":
                total = self._export_stacked(conn)
            else:
                total = self._export_wide(conn)
        finally:
            conn.close()

        logger.info("✓ Export selesai → %s  (%d baris)", self.out_path, total)
        return total

    # ── Internal ──────────────────────────────────────────────────────────────

    def _build_where(self) -> tuple[str, list]:
        """Bangun WHERE clause + params berdasarkan filter yang aktif."""
        clauses: list[str] = []
        params:  list      = []

        # Filter sinyal
        placeholders = ",".join("?" * len(self.signals))
        clauses.append(f"signal IN ({placeholders})")
        params.extend(self.signals)

        if self.node_id is not None:
            clauses.append("node_id = ?")
            params.append(self.node_id)

        if self.from_ms is not None:
            clauses.append("ts_server_ms >= ?")
            params.append(self.from_ms)

        if self.to_ms is not None:
            clauses.append("ts_server_ms <= ?")
            params.append(self.to_ms)

        where = "WHERE " + " AND ".join(clauses) if clauses else ""
        return where, params

    def _build_query(self) -> tuple[str, list]:
        """Bangun SQL query + params berdasarkan filter yang aktif."""
        where, params = self._build_where()

        sql = f"""
            SELECT
                node_id,
                window_num,
                ts_sensor_ms,
                ts_server_ms,
                signal,
                hr,
                spo2,
                finger,
                quality_flag,
                rel_error,
                sparsity,
                snr_db,
                values_json
            FROM windows
            {where}
            ORDER BY node_id, ts_server_ms, signal
        """
        return sql, params

    def _get_max_val_cols(self, conn: sqlite3.Connection) -> int:
        """Dapatkan jumlah maksimum elemen dalam array values_json."""
        where, params = self._build_where()
        sql = f"SELECT MAX(json_array_length(values_json)) FROM windows {where}"
        cursor = conn.execute(sql, params)
        row = cursor.fetchone()
        if row and row[0] is not None:
            return row[0]
        return 0

    def _iter_rows(self, conn: sqlite3.Connection) -> Iterator[sqlite3.Row]:
        """Stream rows dari DB pakai server-side cursor (hemat RAM)."""
        sql, params = self._build_query()
        cursor = conn.execute(sql, params)
        while True:
            batch = cursor.fetchmany(self.chunk)
            if not batch:
                break
            yield from batch

    def _iter_windows(self, conn: sqlite3.Connection) -> Iterator[tuple[tuple, list[sqlite3.Row]]]:
        """Group rows berdasarkan (node_id, window_num) untuk mode stacked."""
        sql, params = self._build_query()
        cursor = conn.execute(sql, params)
        
        current_window = None
        current_rows = []
        
        while True:
            batch = cursor.fetchmany(self.chunk)
            if not batch:
                break
            for row in batch:
                win_key = (row["node_id"], row["window_num"])
                if current_window is None:
                    current_window = win_key
                
                if win_key != current_window:
                    yield current_window, current_rows
                    current_window = win_key
                    current_rows = []
                
                current_rows.append(row)
                
        if current_rows:
            yield current_window, current_rows

    def _export_wide(self, conn: sqlite3.Connection) -> int:
        """Tulis CSV baris per baris. Kembalikan jumlah baris."""
        self.out_path.parent.mkdir(parents=True, exist_ok=True)

        logger.info("Menghitung maksimum kolom sinyal (bisa butuh waktu jika DB besar)...")
        max_val_cols = self._get_max_val_cols(conn)
        logger.info("Maksimum kolom sinyal (val_N) = %d", max_val_cols)

        total          = 0
        header_written = False
        buffer         = []

        def _flush(writer, rows, write_header, ncols):
            nonlocal header_written
            if not rows and not write_header:
                return
            if not header_written and write_header:
                val_headers = [f"val_{i}" for i in range(ncols)]
                writer.writerow([
                    "node_id", "window_num",
                    "ts_sensor_ms", "ts_server_ms",
                    "signal", "hr", "spo2", "finger",
                    "quality_flag", "rel_error", "sparsity", "snr_db",
                    *val_headers,
                ])
                header_written = True
            for r in rows:
                writer.writerow(r)

        with open(self.out_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)

            for row in self._iter_rows(conn):
                values: list[float] = json.loads(row["values_json"])

                record = [
                    row["node_id"],
                    row["window_num"],
                    row["ts_sensor_ms"],
                    row["ts_server_ms"],
                    row["signal"],
                    row["hr"]           if row["hr"]   is not None else "",
                    row["spo2"]         if row["spo2"] is not None else "",
                    int(bool(row["finger"])) if row["finger"] is not None else "",
                    row["quality_flag"] or "",
                    _fmt(row["rel_error"]),
                    _fmt(row["sparsity"]),
                    _fmt(row["snr_db"]),
                    *[_fmt(v) for v in values],
                    *([""] * (max_val_cols - len(values))),
                ]

                buffer.append(record)
                total += 1

                # Tulis header + buffer setelah baris pertama (tahu jumlah val_*)
                if total == 1:
                    _flush(writer, buffer, write_header=True, ncols=max_val_cols)
                    buffer.clear()
                elif len(buffer) >= self.chunk:
                    _flush(writer, buffer, write_header=False, ncols=max_val_cols)
                    buffer.clear()

            # Sisa buffer, atau hanya header jika kosong
            _flush(writer, buffer, write_header=(total == 0), ncols=max_val_cols)

        if total == 0:
            logger.warning("Tidak ada data yang cocok dengan filter yang diberikan.")

        return total

    def _export_stacked(self, conn: sqlite3.Connection) -> int:
        """Tulis CSV dalam format stacked (baris = sample, kolom = sinyal)."""
        self.out_path.parent.mkdir(parents=True, exist_ok=True)
        total = 0
        
        with open(self.out_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            
            # Header
            header = [
                "node_id", "window_num", "ts_sensor_ms", "ts_server_ms", 
                "sample_idx", "hr", "spo2", "finger"
            ]
            header.extend(self.signals)
            writer.writerow(header)
            
            for win_key, rows in self._iter_windows(conn):
                if not rows:
                    continue
                    
                first = rows[0]
                node_id = first["node_id"]
                window_num = first["window_num"]
                ts_sensor_ms = first["ts_sensor_ms"]
                ts_server_ms = first["ts_server_ms"]
                hr = first["hr"] if first["hr"] is not None else ""
                spo2 = first["spo2"] if first["spo2"] is not None else ""
                finger = int(bool(first["finger"])) if first["finger"] is not None else ""
                
                sig_values = {}
                max_len = 0
                for r in rows:
                    sig = r["signal"]
                    vals = json.loads(r["values_json"])
                    sig_values[sig] = vals
                    if len(vals) > max_len:
                        max_len = len(vals)
                
                for i in range(max_len):
                    out_row = [
                        node_id, window_num, ts_sensor_ms, ts_server_ms, 
                        i, hr, spo2, finger
                    ]
                    for sig in self.signals:
                        vals = sig_values.get(sig, [])
                        if i < len(vals):
                            out_row.append(_fmt(vals[i]))
                        else:
                            out_row.append("")
                    writer.writerow(out_row)
                    total += 1
                    
        if total == 0:
            logger.warning("Tidak ada data yang cocok dengan filter yang diberikan.")
            
        return total


# =============================================================================
# Helpers
# =============================================================================

def _fmt(v) -> str:
    """Format float ke string, kosong jika None."""
    if v is None:
        return ""
    try:
        return f"{float(v):.6f}"
    except (TypeError, ValueError):
        return str(v)


# =============================================================================
# CLI
# =============================================================================

def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Export data IMU + PPG dari health_monitor.db ke CSV"
    )
    p.add_argument(
        "--db",
        default = "data/health_monitor.db",
        help    = "Path ke file SQLite (default: data/health_monitor.db)",
    )
    p.add_argument(
        "--out",
        default = "health_export.csv",
        help    = "Path output CSV (default: health_export.csv)",
    )
    p.add_argument(
        "--node",
        type    = int,
        default = None,
        help    = "Filter node_id tertentu (default: semua node)",
    )
    p.add_argument(
        "--signals",
        nargs   = "+",
        default = ALL_SIGNALS,
        choices = ALL_SIGNALS,
        metavar = "SIGNAL",
        help    = (
            f"Sinyal yang di-export. Pilihan: {ALL_SIGNALS}. "
            "Default: semua sinyal."
        ),
    )
    p.add_argument(
        "--from-ms",
        type    = int,
        default = None,
        dest    = "from_ms",
        help    = "Filter mulai dari epoch ms (ts_server_ms >=)",
    )
    p.add_argument(
        "--to-ms",
        type    = int,
        default = None,
        dest    = "to_ms",
        help    = "Filter sampai epoch ms (ts_server_ms <=)",
    )
    p.add_argument(
        "--chunk",
        type    = int,
        default = 500,
        help    = "Ukuran batch query (default: 500, hemat RAM untuk DB besar)",
    )
    p.add_argument(
        "--format",
        choices = ["wide", "stacked"],
        default = "wide",
        dest    = "fmt",
        help    = "Format output: wide (default, menyamping) atau stacked (kolom per sinyal)",
    )
    return p.parse_args()


def main() -> None:
    args = _parse_args()

    logger.info("Sinyal yang di-export : %s", args.signals)
    if args.node:
        logger.info("Filter node_id        : %d", args.node)
    if args.from_ms:
        logger.info("Filter from_ms        : %d", args.from_ms)
    if args.to_ms:
        logger.info("Filter to_ms          : %d", args.to_ms)

    exporter = CSVExporter(
        db_path  = args.db,
        out_path = args.out,
        signals  = args.signals,
        node_id  = args.node,
        from_ms  = args.from_ms,
        to_ms    = args.to_ms,
        chunk    = args.chunk,
        fmt      = args.fmt,
    )
    exporter.run()


if __name__ == "__main__":
    main()
