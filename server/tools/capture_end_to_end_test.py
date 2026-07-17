r"""
Capture end-to-end automated test:
- serial from gateway / node1 / node2
- MQTT topics from broker

Example:
    .\server\.venv\Scripts\python.exe -m server.tools.capture_end_to_end_test ^
        --gateway-port COM7 --imu-port COM15 --ppg-port COM3 --duration 430
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from queue import Empty, Queue

import paho.mqtt.client as mqtt
import serial

try:
    from paho.mqtt.enums import CallbackAPIVersion
    _PAHO_V2 = True
except ImportError:
    _PAHO_V2 = False


TX_IMU_RE = re.compile(
    r"\[TX_IMU\] node=(?P<node>\d+) scenario=(?P<scenario>\d+) cycle=(?P<cycle>\d+)/(?P<repeat>\d+) "
    r"phase=(?P<phase>\S+) seq=(?P<seq>\d+) expect=(?P<expect>\S+) actual=(?P<actual>\S+) "
    r"next_hop=(?P<next_hop>\d+) ok=(?P<ok>\d+) ts=(?P<ts>\d+) t=(?P<t>\d+)"
)
TX_PPG_RE = re.compile(
    r"\[TX_PPG\] node=(?P<node>\d+) scenario=(?P<scenario>\d+) cycle=(?P<cycle>\d+)/(?P<repeat>\d+) "
    r"phase=(?P<phase>\S+) seq=(?P<seq>\d+) ok=(?P<ok>\d+) ts=(?P<ts>\d+) t=(?P<t>\d+)"
)
MQTT_RE = re.compile(
    r"\[MQTT\] route=(?P<route>\S+) topic=(?P<topic>\S+) ok=(?P<ok>\d+) payload_len=(?P<payload_len>\d+) t=(?P<t>\d+)"
)


@dataclass
class Event:
    source: str
    line: str
    host_ts: float


def serial_reader(port: str, label: str, baud: int, queue: Queue, stop_flag: threading.Event) -> None:
    with serial.Serial(port, baud, timeout=1) as ser:
        ser.reset_input_buffer()
        while not stop_flag.is_set():
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                queue.put(Event(label, line, time.time()))


def build_mqtt_client(on_message):
    if _PAHO_V2:
        client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    else:
        client = mqtt.Client()
    client.on_message = on_message
    return client


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-port", required=True)
    parser.add_argument("--imu-port", required=True)
    parser.add_argument("--ppg-port", required=True)
    parser.add_argument("--duration", type=int, default=430)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mqtt-broker", default="localhost")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--topic-base", default="health_monitor")
    parser.add_argument("--out", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = args.out or Path("server/data") / f"end_to_end_capture_{timestamp}"
    out_dir.mkdir(parents=True, exist_ok=True)

    queue: Queue[Event] = Queue()
    stop_flag = threading.Event()

    threads = [
        threading.Thread(target=serial_reader, args=(args.gateway_port, "gateway", args.baud, queue, stop_flag), daemon=True),
        threading.Thread(target=serial_reader, args=(args.imu_port, "imu", args.baud, queue, stop_flag), daemon=True),
        threading.Thread(target=serial_reader, args=(args.ppg_port, "ppg", args.baud, queue, stop_flag), daemon=True),
    ]
    for th in threads:
        th.start()

    mqtt_rows: list[dict] = []
    tx_imu_rows: list[dict] = []
    tx_ppg_rows: list[dict] = []
    serial_mqtt_rows: list[dict] = []
    raw_log = out_dir / "serial_and_mqtt_raw.log"

    def on_mqtt_message(client, userdata, message):
        payload = message.payload.decode("utf-8", errors="replace")
        row = {
            "host_ts": datetime.now().isoformat(timespec="seconds"),
            "topic": message.topic,
            "payload_len": len(payload),
            "payload": payload,
        }
        mqtt_rows.append(row)

    client = build_mqtt_client(on_mqtt_message)
    client.connect(args.mqtt_broker, args.mqtt_port, keepalive=60)
    client.subscribe(f"{args.topic_base}/+/cs_imu")
    client.subscribe(f"{args.topic_base}/+/cs_ppg")
    client.loop_start()

    start = time.time()
    print(f"Mulai capture end-to-end ke {out_dir}")

    with raw_log.open("w", encoding="utf-8") as raw_fp:
        while time.time() - start < args.duration:
            try:
                event = queue.get(timeout=1)
            except Empty:
                continue

            iso_ts = datetime.fromtimestamp(event.host_ts).isoformat(timespec="seconds")
            safe_line = event.line.encode("utf-8", errors="replace").decode(
                sys.stdout.encoding or "utf-8", errors="backslashreplace"
            )
            raw_fp.write(f"{iso_ts} [{event.source}] {event.line}\n")
            raw_fp.flush()
            print(f"[{event.source}] {safe_line}")

            match = TX_IMU_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                tx_imu_rows.append(row)
                continue

            match = TX_PPG_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                tx_ppg_rows.append(row)
                continue

            match = MQTT_RE.match(event.line)
            if match:
                row = match.groupdict()
                row["source"] = event.source
                serial_mqtt_rows.append(row)

    stop_flag.set()
    for th in threads:
        th.join(timeout=2)
    client.loop_stop()
    client.disconnect()

    write_csv(out_dir / "summary_tx_imu.csv", tx_imu_rows)
    write_csv(out_dir / "summary_tx_ppg.csv", tx_ppg_rows)
    write_csv(out_dir / "summary_gateway_mqtt_publish.csv", serial_mqtt_rows)
    write_csv(out_dir / "summary_broker_receive.csv", mqtt_rows)

    broker_topics = {}
    for row in mqtt_rows:
        broker_topics[row["topic"]] = broker_topics.get(row["topic"], 0) + 1

    report = {
        "ports": {
            "gateway": args.gateway_port,
            "imu": args.imu_port,
            "ppg": args.ppg_port,
        },
        "mqtt_broker": f"{args.mqtt_broker}:{args.mqtt_port}",
        "duration_s": args.duration,
        "tx_imu_total": len(tx_imu_rows),
        "tx_ppg_total": len(tx_ppg_rows),
        "gateway_mqtt_publish_total": len(serial_mqtt_rows),
        "broker_receive_total": len(mqtt_rows),
        "broker_topics": broker_topics,
    }

    (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (out_dir / "README_HASIL_END_TO_END.txt").write_text(
        "\n".join(
            [
                "HASIL UJI END-TO-END OTOMATIS",
                f"Folder              : {out_dir}",
                f"Gateway port        : {args.gateway_port}",
                f"IMU port            : {args.imu_port}",
                f"PPG/relay port      : {args.ppg_port}",
                f"Broker MQTT         : {args.mqtt_broker}:{args.mqtt_port}",
                f"Durasi capture      : {args.duration} detik",
                f"Total TX IMU        : {len(tx_imu_rows)}",
                f"Total TX PPG        : {len(tx_ppg_rows)}",
                f"Total publish GW    : {len(serial_mqtt_rows)}",
                f"Total diterima broker: {len(mqtt_rows)}",
                f"Topik broker        : {json.dumps(broker_topics)}",
            ]
        ),
        encoding="utf-8",
    )
    print(json.dumps(report, indent=2))
    print(f"Selesai. Hasil disimpan ke {out_dir}")


if __name__ == "__main__":
    main()
