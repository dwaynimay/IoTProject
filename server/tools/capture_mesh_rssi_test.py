r"""
Capture serial logs from three ESP32 nodes during the automated RSSI mesh test
and summarize direct vs relay routing results.

Example:
    .\server\.venv\Scripts\python.exe -m server.tools.capture_mesh_rssi_test ^
        --gateway-port COM7 --imu-port COM15 --ppg-port COM3 --duration 430
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from queue import Queue, Empty

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


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def stddev(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    mu = mean(values)
    return math.sqrt(sum((v - mu) ** 2 for v in values) / len(values))


def reader_thread(port: str, label: str, baud: int, queue: Queue, stop_flag: threading.Event) -> None:
    with serial.Serial(port, baud, timeout=1) as ser:
        ser.reset_input_buffer()
        while not stop_flag.is_set():
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode("utf-8", errors="replace").strip()
            except Exception:
                continue
            if line:
                queue.put(Event(source=label, line=line, host_ts=time.time()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-port", required=True)
    parser.add_argument("--imu-port", required=True)
    parser.add_argument("--ppg-port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=430,
                        help="Durasi capture dalam detik. 5 cycle x 4 phase x 20 dtk + buffer.")
    parser.add_argument("--out", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = args.out or Path("server/data") / f"mesh_rssi_capture_{timestamp}"
    out_dir.mkdir(parents=True, exist_ok=True)

    queue: Queue[Event] = Queue()
    stop_flag = threading.Event()
    labels = {
        "gateway": args.gateway_port,
        "imu": args.imu_port,
        "ppg": args.ppg_port,
    }

    threads = [
        threading.Thread(target=reader_thread,
                         args=(port, label, args.baud, queue, stop_flag),
                         daemon=True)
        for label, port in labels.items()
    ]
    for th in threads:
        th.start()

    raw_log = out_dir / "serial_raw.log"
    tx_rows: list[dict] = []
    phase_rows: list[dict] = []
    gw_rows: list[dict] = []
    done_rows: list[dict] = []

    start = time.time()
    print(f"Mulai capture mesh RSSI ke {out_dir}")

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
    for th in threads:
        th.join(timeout=2)

    write_csv(out_dir / "summary_tx.csv", tx_rows)
    write_csv(out_dir / "summary_gateway.csv", gw_rows)
    write_csv(out_dir / "summary_phase.csv", phase_rows)

    success_count = 0
    route_counts = {"DIRECT": 0, "RELAY": 0}
    tx_seq_set: set[int] = set()
    tx_bytes_total = 0
    raw_bytes_ref = 0
    cs_bytes_ref = 0
    for row in tx_rows:
        if row["expect"] == row["actual"] and row["ok"] == "1":
            success_count += 1
        route_counts[row["actual"]] = route_counts.get(row["actual"], 0) + 1
        tx_seq_set.add(int(row["seq"]))
        tx_bytes_total += int(row["pkt_bytes"])
        raw_bytes_ref = int(row["raw_bytes"])
        cs_bytes_ref = int(row["cs_bytes"])

    gw_seq_set: set[int] = set()
    gw_bytes_total = 0
    route_rx_counts = {"DIRECT": 0, "RELAYED": 0}
    latency_all: list[float] = []
    latency_direct: list[float] = []
    latency_relay: list[float] = []
    for row in gw_rows:
        seq = int(row["seq"])
        gw_seq_set.add(seq)
        gw_bytes_total += int(row["pkt_bytes"])
        route_rx_counts[row["route"]] = route_rx_counts.get(row["route"], 0) + 1
        lat = float(row["latency_ms"])
        latency_all.append(lat)
        if row["route"] == "DIRECT":
            latency_direct.append(lat)
        else:
            latency_relay.append(lat)

    lost_sequences = sorted(tx_seq_set - gw_seq_set)
    received_sequences = sorted(gw_seq_set)
    packet_loss_count = len(lost_sequences)
    packet_loss_pct = round((packet_loss_count / len(tx_seq_set)) * 100.0, 2) if tx_seq_set else 0.0
    throughput_tx_bps = round(tx_bytes_total / args.duration, 2) if args.duration else 0.0
    throughput_rx_bps = round(gw_bytes_total / args.duration, 2) if args.duration else 0.0
    compression_pct = round(((raw_bytes_ref - cs_bytes_ref) / raw_bytes_ref) * 100.0, 2) if raw_bytes_ref else 0.0

    latency_rows = [
        {
            "route": "ALL",
            "count": len(latency_all),
            "avg_ms": round(mean(latency_all), 2),
            "min_ms": round(min(latency_all), 2) if latency_all else 0.0,
            "max_ms": round(max(latency_all), 2) if latency_all else 0.0,
            "std_ms": round(stddev(latency_all), 2),
        },
        {
            "route": "DIRECT",
            "count": len(latency_direct),
            "avg_ms": round(mean(latency_direct), 2),
            "min_ms": round(min(latency_direct), 2) if latency_direct else 0.0,
            "max_ms": round(max(latency_direct), 2) if latency_direct else 0.0,
            "std_ms": round(stddev(latency_direct), 2),
        },
        {
            "route": "RELAYED",
            "count": len(latency_relay),
            "avg_ms": round(mean(latency_relay), 2),
            "min_ms": round(min(latency_relay), 2) if latency_relay else 0.0,
            "max_ms": round(max(latency_relay), 2) if latency_relay else 0.0,
            "std_ms": round(stddev(latency_relay), 2),
        },
    ]
    write_csv(out_dir / "summary_latency.csv", latency_rows)

    packet_loss_rows = [
        {
            "tx_total": len(tx_seq_set),
            "rx_total": len(gw_seq_set),
            "lost_total": packet_loss_count,
            "packet_loss_pct": packet_loss_pct,
            "lost_seq": " ".join(map(str, lost_sequences[:50])),
            "received_seq": " ".join(map(str, received_sequences[:50])),
        }
    ]
    write_csv(out_dir / "summary_packet_loss.csv", packet_loss_rows)

    bandwidth_rows = [
        {
            "side": "TX_NODE",
            "packet_count": len(tx_rows),
            "total_bytes": tx_bytes_total,
            "throughput_Bps": throughput_tx_bps,
        },
        {
            "side": "GW_RX",
            "packet_count": len(gw_rows),
            "total_bytes": gw_bytes_total,
            "throughput_Bps": throughput_rx_bps,
        },
    ]
    write_csv(out_dir / "summary_bandwidth.csv", bandwidth_rows)

    efficiency_rows = [
        {
            "raw_window_bytes": raw_bytes_ref,
            "cs_measurement_bytes": cs_bytes_ref,
            "saved_bytes": raw_bytes_ref - cs_bytes_ref,
            "compression_pct": compression_pct,
            "compression_ratio": round(raw_bytes_ref / cs_bytes_ref, 3) if cs_bytes_ref else 0.0,
        }
    ]
    write_csv(out_dir / "summary_efficiency.csv", efficiency_rows)

    report = {
        "ports": labels,
        "duration_s": args.duration,
        "tx_total": len(tx_rows),
        "gateway_total": len(gw_rows),
        "phase_total": len(phase_rows),
        "done_total": len(done_rows),
        "tx_expect_match_ok": success_count,
        "tx_success_rate_pct": round((success_count / len(tx_rows)) * 100.0, 2) if tx_rows else 0.0,
        "route_counts_tx": route_counts,
        "route_counts_gateway": route_rx_counts,
        "packet_loss_count": packet_loss_count,
        "packet_loss_pct": packet_loss_pct,
        "throughput_tx_Bps": throughput_tx_bps,
        "throughput_rx_Bps": throughput_rx_bps,
        "latency_avg_ms": round(mean(latency_all), 2),
        "latency_direct_avg_ms": round(mean(latency_direct), 2),
        "latency_relay_avg_ms": round(mean(latency_relay), 2),
        "efficiency_compression_pct": compression_pct,
    }

    (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (out_dir / "README_HASIL_MESH.txt").write_text(
        "\n".join(
            [
                "HASIL UJI ESP-NOW MESH RSSI OTOMATIS",
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
                f"Packet loss      : {packet_loss_count} ({packet_loss_pct}%)",
                f"Latency avg      : {report['latency_avg_ms']} ms",
                f"Latency direct   : {report['latency_direct_avg_ms']} ms",
                f"Latency relay    : {report['latency_relay_avg_ms']} ms",
                f"TX throughput    : {throughput_tx_bps} Bps",
                f"RX throughput    : {throughput_rx_bps} Bps",
                f"Efisiensi data   : {compression_pct}%",
            ]
        ),
        encoding="utf-8",
    )
    print(json.dumps(report, indent=2))
    print(f"Selesai. Hasil disimpan ke {out_dir}")


if __name__ == "__main__":
    main()
