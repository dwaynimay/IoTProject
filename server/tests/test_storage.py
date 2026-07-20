# File: server/tests/test_storage.py

# =============================================================================
# test_storage.py — Unit test untuk core/storage.py
# =============================================================================
#
# Jalankan dari root project:
#   python -m pytest server/tests/test_storage.py -v
#
# Atau tanpa pytest:
#   python server/tests/test_storage.py
# =============================================================================

import sys
import os
import json
import sqlite3
import time
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    from core.storage import StorageManager, _now_ms
except ImportError as e:
    print(f"[SKIP] Tidak bisa import storage: {e}")
    sys.exit(0)

# Stub WindowReport ringan — tidak butuh import quality.py penuh
class _FakeMetric:
    def __init__(self, rel_error=0.05, sparsity=0.30, snr_db=22.0, flag="OK"):
        self.relative_error = rel_error
        self.sparsity_ratio = sparsity
        self.snr_db         = snr_db
        class _Flag:
            value = flag
        self.flag = _Flag()

class _FakeReport:
    def __init__(self, signals, rel_error=0.05):
        self.metrics = {sig: _FakeMetric(rel_error=rel_error) for sig in signals}


# =============================================================================
# Fixtures — helper buat DB in-memory
# =============================================================================

def _make_db() -> StorageManager:
    """DB di file temporer — diisolasi per test."""
    tmp = tempfile.mktemp(suffix=".db")
    db  = StorageManager(db_path=tmp, retention_hours=24)
    db.open()
    return db

def _make_results(signals=("ax", "gz", "ir"), n=64) -> dict:
    """Buat hasil rekonstruksi dummy."""
    rng = np.random.default_rng(0)
    return {sig: rng.standard_normal(n) for sig in signals}


# =============================================================================
# Test open / close
# =============================================================================

def test_open_creates_tables():
    db = _make_db()
    # Cek tabel ada
    tables = db._conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'"
    ).fetchall()
    names = [t[0] for t in tables]
    assert "windows" in names, f"Tabel windows tidak ada: {names}"
    assert "events"  in names, f"Tabel events tidak ada: {names}"
    db.close()

def test_double_open_idempotent():
    """open() dua kali tidak crash — tabel CREATE IF NOT EXISTS."""
    with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as f:
        path = f.name
    db1 = StorageManager(db_path=path)
    db1.open()
    db1.close()
    db2 = StorageManager(db_path=path)
    db2.open()
    db2.close()


def test_open_migrates_legacy_windows_table():
    path = tempfile.mktemp(suffix=".db")
    conn = sqlite3.connect(path)
    conn.execute(
        """
        CREATE TABLE windows (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id INTEGER NOT NULL, window_num INTEGER NOT NULL,
            ts_sensor_ms INTEGER NOT NULL, ts_server_ms INTEGER NOT NULL,
            signal TEXT NOT NULL, values_json TEXT NOT NULL,
            rel_error REAL, sparsity REAL, snr_db REAL, quality_flag TEXT,
            hr INTEGER, spo2 REAL, finger INTEGER
        )
        """
    )
    conn.commit()
    conn.close()

    db = StorageManager(db_path=path)
    db.open()
    columns = {row[1] for row in db._conn.execute("PRAGMA table_info(windows)")}
    assert "session_id" in columns
    assert db.save_window(1, 1, 100, _make_results(("ax",))) == 1
    db.close()

def test_ensure_open_raises_if_not_opened():
    db = StorageManager(db_path=":memory:")
    # Jangan panggil open() — langsung akses
    try:
        db.get_all_node_ids()
        assert False, "Harusnya RuntimeError"
    except RuntimeError as e:
        assert "open()" in str(e)

def test_context_manager():
    tmp = tempfile.mktemp(suffix=".db")
    with StorageManager(db_path=tmp) as db:
        assert db._conn is not None
    assert db._conn is None  # close() dipanggil otomatis


# =============================================================================
# Test save_window
# =============================================================================

def test_save_window_inserts_rows():
    db      = _make_db()
    results = _make_results(("ax", "gz", "ir"))
    count   = db.save_window(
        node_id=1, window_num=1, ts_sensor=1000, results=results
    )
    assert count == 3  # 3 sinyal = 3 baris
    db.close()

def test_save_window_values_roundtrip():
    """Nilai ndarray harus bisa dibaca kembali dari DB."""
    db      = _make_db()
    x_hat   = np.array([1.5, -2.3, 0.0, 4.7] + [0.0] * 60)
    results = {"ax": x_hat}

    db.save_window(node_id=1, window_num=1, ts_sensor=500, results=results)

    rows = db.get_last_windows(node_id=1, signal="ax", n=1)
    assert len(rows) == 1
    vals = rows[0]["values"]
    np.testing.assert_allclose(vals[:4], [1.5, -2.3, 0.0, 4.7], rtol=1e-6)
    db.close()

def test_save_window_with_report():
    """Metrik dari WindowReport harus tersimpan."""
    db      = _make_db()
    results = _make_results(("gx",))
    report  = _FakeReport(("gx",), rel_error=0.08)

    db.save_window(node_id=2, window_num=3, ts_sensor=9000,
                   results=results, report=report, hr=72, spo2=98.1, finger=True)

    rows = db.get_last_windows(node_id=2, signal="gx", n=1)
    assert len(rows) == 1
    r = rows[0]
    assert r["window_num"]   == 3
    assert r["hr"]           == 72
    assert abs(r["spo2"] - 98.1) < 0.01
    assert r["finger"]       is True
    assert r["quality_flag"] == "OK"
    assert r["rel_error"]    is not None
    db.close()

def test_save_window_without_report():
    """Tanpa report, kolom metrik harus NULL."""
    db      = _make_db()
    results = _make_results(("ay",))

    db.save_window(node_id=1, window_num=1, ts_sensor=100, results=results)

    rows = db.get_last_windows(node_id=1, signal="ay", n=1)
    assert rows[0]["rel_error"]    is None
    assert rows[0]["quality_flag"] is None
    db.close()

def test_save_multiple_windows_sequential():
    db      = _make_db()
    results = _make_results(("ax",))

    for i in range(5):
        db.save_window(node_id=1, window_num=i+1, ts_sensor=i*640, results=results)

    rows = db.get_last_windows(node_id=1, signal="ax", n=10)
    assert len(rows) == 5
    # Harus urut berdasarkan id insert (kronologis insert)
    nums = [r["window_num"] for r in rows]
    # window_num 1..5, dan ORDER BY id DESC lalu reversed → harus ascending
    assert nums == sorted(nums), f"Tidak terurut: {nums}"
    db.close()


# =============================================================================
# Test log_event / get_last_events
# =============================================================================

def test_log_event_basic():
    db = _make_db()
    db.log_event(node_id=1, event_type="LOW_QUALITY", detail="gx gz")

    events = db.get_last_events(n=5)
    assert len(events) == 1
    assert events[0]["event_type"] == "LOW_QUALITY"
    assert events[0]["detail"]     == "gx gz"
    db.close()

def test_log_event_filter_by_node():
    db = _make_db()
    db.log_event(node_id=1, event_type="CRITICAL", detail="ax")
    db.log_event(node_id=2, event_type="LOW_QUALITY", detail="gz")

    events_node1 = db.get_last_events(node_id=1)
    assert len(events_node1) == 1
    assert events_node1[0]["node_id"] == 1
    db.close()

def test_log_event_filter_by_type():
    db = _make_db()
    db.log_event(node_id=1, event_type="CRITICAL",    detail="a")
    db.log_event(node_id=1, event_type="LOW_QUALITY", detail="b")
    db.log_event(node_id=1, event_type="CRITICAL",    detail="c")

    crits = db.get_last_events(event_type="CRITICAL")
    assert len(crits) == 2
    assert all(e["event_type"] == "CRITICAL" for e in crits)
    db.close()

def test_log_event_multiple_nodes():
    db = _make_db()
    for node in (1, 2, 1, 2, 1):
        db.log_event(node_id=node, event_type="TEST", detail="")

    all_events = db.get_last_events(n=10)
    assert len(all_events) == 5
    db.close()


# =============================================================================
# Test get_node_stats
# =============================================================================

def test_get_node_stats_empty():
    db    = _make_db()
    stats = db.get_node_stats(node_id=99)
    assert stats["total_windows"]    == 0
    assert stats["low_quality_count"] == 0
    assert stats["avg_rel_error"]    == 0.0
    db.close()

def test_get_node_stats_counts():
    db      = _make_db()
    signals = ("ax", "ir")

    # Window 1: OK
    report_ok = _FakeReport(signals, rel_error=0.05)
    db.save_window(node_id=1, window_num=1, ts_sensor=100,
                   results=_make_results(signals), report=report_ok, hr=70, spo2=98.0)

    # Window 2: LOW_QUALITY
    class _LQMetric(_FakeMetric):
        def __init__(self):
            super().__init__(rel_error=0.30, flag="LOW_QUALITY")
    class _LQReport:
        metrics = {s: _LQMetric() for s in signals}
    db.save_window(node_id=1, window_num=2, ts_sensor=740,
                   results=_make_results(signals), report=_LQReport(), hr=72, spo2=97.5)

    stats = db.get_node_stats(node_id=1)

    assert stats["total_windows"]     == 2
    assert stats["low_quality_count"] == 1
    assert stats["avg_rel_error"]     > 0
    assert stats["last_hr"]           == 72  # window terakhir
    db.close()


def test_stats_distinguish_same_window_number_across_sessions():
    path = tempfile.mktemp(suffix=".db")
    results = _make_results(("ax", "ir"))

    first = StorageManager(db_path=path)
    first.open()
    first.save_window(1, 1, 100, results)
    first.close()

    second = StorageManager(db_path=path)
    second.open()
    second.save_window(1, 1, 200, results)
    assert second.get_node_stats(1)["total_windows"] == 2
    second.close()


def test_activity_rows_are_ordered_by_server_time_not_window_number():
    db = _make_db()
    results = _make_results(("ir",))
    db.save_window(1, 99, 100, results)
    db.save_window(1, 1, 200, results)
    ids = db._conn.execute("SELECT id FROM windows ORDER BY id").fetchall()
    db._conn.execute("UPDATE windows SET ts_server_ms = 2000 WHERE id = ?", ids[0])
    db._conn.execute("UPDATE windows SET ts_server_ms = 3000 WHERE id = ?", ids[1])
    db._conn.commit()

    rows = db.get_activity_rows(1, cutoff_ms=0)
    assert [row[0] for row in rows] == [2000, 3000]
    db.close()

def test_get_all_node_ids():
    db = _make_db()
    for node_id in (1, 2, 1, 3):
        db.save_window(
            node_id=node_id, window_num=1, ts_sensor=0,
            results=_make_results(("ax",))
        )

    ids = db.get_all_node_ids()
    assert set(ids) == {1, 2, 3}
    db.close()


# =============================================================================
# Test get_last_windows — edge cases
# =============================================================================

def test_get_last_windows_empty():
    db   = _make_db()
    rows = db.get_last_windows(node_id=99, signal="ax", n=10)
    assert rows == []
    db.close()

def test_get_last_windows_limit():
    db = _make_db()
    for i in range(15):
        db.save_window(
            node_id=1, window_num=i+1, ts_sensor=i*100,
            results=_make_results(("gz",))
        )

    rows = db.get_last_windows(node_id=1, signal="gz", n=5)
    assert len(rows) == 5
    # 5 window terakhir = window_num 11..15 (insert terakhir berdasarkan id)
    nums = [r["window_num"] for r in rows]
    assert min(nums) >= 11, f"Harusnya 5 window terakhir (11-15), dapat: {nums}"
    assert nums == sorted(nums), f"Harus urut ascending: {nums}"
    db.close()

def test_get_last_windows_correct_signal_filter():
    """get_last_windows harus filter signal dengan benar."""
    db = _make_db()
    db.save_window(
        node_id=1, window_num=1, ts_sensor=0,
        results=_make_results(("ax", "gx", "ir"))
    )

    rows_ax = db.get_last_windows(node_id=1, signal="ax", n=10)
    rows_ir = db.get_last_windows(node_id=1, signal="ir", n=10)

    assert len(rows_ax) == 1
    assert len(rows_ir) == 1
    # Pastikan tidak tercampur
    rows_xy = db.get_last_windows(node_id=1, signal="ay", n=10)
    assert len(rows_xy) == 0  # "ay" tidak disimpan
    db.close()


# =============================================================================
# Test purge_old
# =============================================================================

def test_purge_old_removes_expired():
    db = _make_db()

    # Sisipkan langsung dengan ts_server_ms lama (2 jam lalu)
    old_ms = _now_ms() - 2 * 3_600_000
    db._conn.execute(
        """
        INSERT INTO windows
            (node_id, window_num, ts_sensor_ms, ts_server_ms,
             signal, values_json, hr, spo2, finger)
        VALUES (1, 0, 0, ?, 'ax', '[0.0]', -1, 0.0, 0)
        """,
        (old_ms,),
    )
    db._conn.commit()

    # Sisipkan data baru
    db.save_window(node_id=1, window_num=1, ts_sensor=100,
                   results=_make_results(("ax",)))

    assert db.get_last_windows(node_id=1, signal="ax", n=10).__len__() == 2

    # Purge dengan retention 1 jam
    deleted = db.purge_old(max_age_hours=1)

    assert deleted >= 1
    rows = db.get_last_windows(node_id=1, signal="ax", n=10)
    assert len(rows) == 1  # hanya yang baru tersisa
    db.close()

def test_purge_old_keeps_recent():
    db = _make_db()
    db.save_window(node_id=1, window_num=1, ts_sensor=0,
                   results=_make_results(("ax",)))

    deleted = db.purge_old(max_age_hours=24)
    assert deleted == 0  # data baru tidak terhapus

    rows = db.get_last_windows(node_id=1, signal="ax", n=5)
    assert len(rows) == 1
    db.close()

def test_purge_old_events():
    db     = _make_db()
    old_ms = _now_ms() - 5 * 3_600_000

    db._conn.execute(
        "INSERT INTO events (node_id, ts_server_ms, event_type, detail) VALUES (1, ?, 'OLD', '')",
        (old_ms,),
    )
    db._conn.commit()
    db.log_event(node_id=1, event_type="NEW", detail="")

    deleted = db.purge_old(max_age_hours=1)
    assert deleted >= 1

    events = db.get_last_events(n=10)
    assert all(e["event_type"] != "OLD" for e in events)
    db.close()


# =============================================================================
# Test db_size_bytes
# =============================================================================

def test_db_size_bytes_positive_after_write():
    db = _make_db()
    db.save_window(node_id=1, window_num=1, ts_sensor=0,
                   results=_make_results(("ax",)))
    size = db.db_size_bytes()
    assert size > 0
    db.close()


# =============================================================================
# Runner manual
# =============================================================================

if __name__ == "__main__":
    tests = [
        # open/close
        test_open_creates_tables,
        test_double_open_idempotent,
        test_ensure_open_raises_if_not_opened,
        test_context_manager,
        # save_window
        test_save_window_inserts_rows,
        test_save_window_values_roundtrip,
        test_save_window_with_report,
        test_save_window_without_report,
        test_save_multiple_windows_sequential,
        # log_event
        test_log_event_basic,
        test_log_event_filter_by_node,
        test_log_event_filter_by_type,
        test_log_event_multiple_nodes,
        # get_node_stats
        test_get_node_stats_empty,
        test_get_node_stats_counts,
        test_get_all_node_ids,
        # get_last_windows
        test_get_last_windows_empty,
        test_get_last_windows_limit,
        test_get_last_windows_correct_signal_filter,
        # purge_old
        test_purge_old_removes_expired,
        test_purge_old_keeps_recent,
        test_purge_old_events,
        # misc
        test_db_size_bytes_positive_after_write,
    ]

    passed = failed = 0
    for fn in tests:
        try:
            fn()
            print(f"  ✓  {fn.__name__}")
            passed += 1
        except Exception as e:
            print(f"  ✗  {fn.__name__} → {e}")
            import traceback; traceback.print_exc()
            failed += 1

    print(f"\n{'='*60}")
    print(f"  {passed} passed  |  {failed} failed  |  {len(tests)} total")
    print(f"{'='*60}")
    if failed > 0:
        sys.exit(1)
