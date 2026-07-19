r"""
Capture serial logs from three ESP32 nodes during the automated RSSI relay test
and summarize direct to relay route switching behavior.

Example:
    .\server\.venv\Scripts\python.exe -m server.tools.capture_mesh_relay_test ^
        --gateway-port COM7 --imu-port COM15 --ppg-port COM3 --duration 430
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from queue import Empty, Queue
import re

import serial


TX_RE = re.compile(
    r"\[TX\] node=(?P<node>\d+) scenario=(?P<scenario>\d+) cycle=(?P<cycle>\d+)/(?P<repeat>\d+) "
    r"phase=(?P<phase>\S+) phase_idx=(?P<phase_idx>\d+) seq=(?P<seq>\d+) expect=(?P<expect>\S+) actual=(?P<actual>\S+) "
    r"self=(?P<self>-?\d+) neighbor=(?P<neighbor>-?\d+) next_hop=(?P<next_hop>\d+) ok=(?P<ok>\d+) "
    r"raw_bytes=(?P<raw_bytes>\d+) cs_bytes=(?P<cs_bytes>\d+) pkt_bytes=(?P<pkt_bytes>\d+) "
    r"saved_bytes=(?P<saved_bytes>-?\d+) comp_pct=(?P<comp_pct>[0-9.]+) t=(?P<t>\d+)"
)
GW_RE = re.compile(
    r"\[GW\] route=(?P<route>\S+) node=(?P<node>\d+) scenario=(?P<scenario>\d+) cycle=(?P<cycle>\d+) "
    r"phase_idx=(?P<phase_idx>\d+) seq=(?P<seq>\d+) latency_ms=(?P<latency_ms>\d+) "
    r"raw_bytes=(?P<raw_bytes>\d+) cs_bytes=(?P<cs_bytes>\d+) pkt_bytes=(?P<pkt_bytes>\d+) "
    r"topic=(?P<topic>\S+) payload=(?P<payload>.+)"
)
PHASE_RE = re.compile(
    r"\[PHASE\] node=(?P<node>\d+) scenario=(?P<scenario>\d+) cycle=(?P<cycle>\d+)/(?P<repeat>\d+) "
    r"idx=(?P<idx>\d+) name=(?P<name>\S+) self=(?P<self>-?\d+) neighbor=(?P<neighbor>-?\d+) expect=(?P<expect>\S+) t=(?P<t>\d+)"
)
DONE_RE = re.compile(r"\[DONE\] node=(?P<node>\d+) cycles=(?P<cycles>\d+) tx=(?P<tx>\d+) t=(?P<t>\d+)")


@dataclass
class Event:
    source: str
    line: str
    host_ts: float


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def reader_thread(port: str, label: str, baud: int, queue: Queue, stop_flag: threading.Event) -> None:
    with serial.Serial(port, baud, timeout=1) as ser:
        ser.reset_input_buffer()
        while not stop_flag.is_set():
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                queue.put(Event(source=label, line=line, host_ts=time.time()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-port", required=True)
    parser.add_argument("--imu-port", required=True)
    parser.add_argument("--ppg-port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=430)
    parser.add_argument("--out", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = args.out or Path("server/data") / f"mesh_relay_capture_{timestamp}"
    out_dir.mkdir(parents=True, exist_ok=True)

    queue: Queue[Event] = Queue()
    stop_flag = threading.Event()
    labels = {
        "gateway": args.gateway_port,
        "imu": args.imu_port,
        "ppg": args.ppg_port,
    }

    threads = [
        threading.Thread(
            target=reader_thread,
            args=(port, label, args.baud, queue, stop_flag),
            daemon=True,
        )
        for label, port in labels.items()
    ]
    for thread in threads:
        thread.start()

    raw_log = out_dir / "serial_raw.log"
    tx_rows: list[dict] = []
    phase_rows: list[dict] = []
    gw_rows: list[dict] = []
    done_rows: list[dict] = []

    start = time.time()
    print(f"Mulai capture mesh relay ke {out_dir}")

    with raw_log.open("w", encoding="utf-8") as raw_fp:
        while time.time() - start < args.duration:
            try:
                event = queue.get(timeout=1)
            except Empty:
                continue

            iso_ts = datetime.fromtimestamp(event.host_ts).isoformat(timespec="seconds")
            raw_fp.write(f"{iso_ts} [{event.source}] {event.line}\n")
            raw_fp.flush()
            safe_line = event.line.encode("utf-8", errors="replace").decode(
                sys.stdout.encoding or "utf-8", errors="backslashreplace"
            )
            print(f"[{event.source}] {safe_line}")

            match = TX_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                tx_rows.append(row)
                continue

            match = PHASE_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                phase_rows.append(row)
                continue

            match = GW_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                gw_rows.append(row)
                continue

            match = DONE_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                done_rows.append(row)

    stop_flag.set()
    for thread in threads:
        thread.join(timeout=2)

    write_csv(out_dir / "summary_tx.csv", tx_rows)
    write_csv(out_dir / "summary_gateway.csv", gw_rows)
    write_csv(out_dir / "summary_phase.csv", phase_rows)
    write_csv(out_dir / "summary_done.csv", done_rows)

    success_count = 0
    route_counts = {"DIRECT": 0, "RELAY": 0}
    phase_counts: dict[str, int] = {}
    for row in tx_rows:
        if row["expect"] == row["actual"] and row["ok"] == "1":
            success_count += 1
        route_counts[row["actual"]] = route_counts.get(row["actual"], 0) + 1
        phase_counts[row["phase"]] = phase_counts.get(row["phase"], 0) + 1

    gateway_route_counts = {"DIRECT": 0, "RELAYED": 0}
    for row in gw_rows:
        gateway_route_counts[row["route"]] = gateway_route_counts.get(row["route"], 0) + 1

    report = {
        "ports": labels,
        "duration_s": args.duration,
        "tx_total": len(tx_rows),
        "gateway_total": len(gw_rows),
        "phase_total": len(phase_rows),
        "done_total": len(done_rows),
        "tx_expect_match_ok": success_count,
        "tx_success_rate_pct": round((success_count / len(tx_rows)) * 100.0, 2) if tx_rows else 0.0,
        "route_counts": route_counts,
        "gateway_route_counts": gateway_route_counts,
        "phase_counts": phase_counts,
    }

    (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (out_dir / "README_HASIL_MESH.txt").write_text(
        "\n".join(
            [
                "HASIL UJI ESP-NOW RELAY MESH RSSI OTOMATIS",
                f"Folder           : {out_dir}",
                f"Gateway port     : {args.gateway_port}",
                f"IMU sender port  : {args.imu_port}",
                f"PPG relay port   : {args.ppg_port}",
                f"Durasi capture   : {args.duration} detik",
                f"Total TX         : {len(tx_rows)}",
                f"Total RX gateway : {len(gw_rows)}",
                f"Match expect=actual + ok=1 : {success_count}",
                f"Success rate     : {report['tx_success_rate_pct']}%",
                f"Route DIRECT     : {route_counts.get('DIRECT', 0)}",
                f"Route RELAY      : {route_counts.get('RELAY', 0)}",
                f"Fase baseline_direct : {phase_counts.get('baseline_direct', 0)}",
                f"Fase forced_relay    : {phase_counts.get('forced_relay', 0)}",
                f"Fase relay_hold      : {phase_counts.get('relay_hold', 0)}",
                f"Fase direct_recovery : {phase_counts.get('direct_recovery', 0)}",
            ]
        ),
        encoding="utf-8",
    )

    print(json.dumps(report, indent=2))
    print(f"Selesai. Hasil disimpan ke {out_dir}")


if __name__ == "__main__":
    main()
