#!/usr/bin/env python3
"""Read Linux CPU, memory, pressure and interface counters into JSON Lines.

This is a resource collector, not a media load generator or quality benchmark.
It changes no host settings and collects no addresses or application content.
"""

import argparse
import contextlib
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import platform
import sys
import time


def positive_interval(value):
    number = float(value)
    if not math.isfinite(number) or number < 0.1:
        raise argparse.ArgumentTypeError("interval must be finite and at least 0.1 seconds")
    return number


def positive_count(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("samples must be positive")
    return number


def parse_cpu(text):
    counters = {}
    for line in text.splitlines():
        fields = line.split()
        if fields and (fields[0] == "cpu" or
                       (fields[0].startswith("cpu") and fields[0][3:].isdigit())):
            # guest/guest_nice already contribute to user/nice: do not add twice.
            values = tuple(int(value) for value in fields[1:9])
            if len(values) != 8:
                raise ValueError("expected eight primary Linux CPU counters")
            counters[fields[0]] = values
    if "cpu" not in counters:
        raise ValueError("aggregate CPU counters unavailable")
    return counters


def cpu_usage(previous, current):
    if previous is None:
        return None
    delta = [new - old for old, new in zip(previous, current)]
    total = sum(delta)
    # Linux documents that iowait can decrease; don't fabricate a valid interval.
    if total <= 0 or any(value < 0 for value in delta):
        return None
    return {
        "busy_percent": round(100 * (total - delta[3] - delta[4] - delta[7]) / total, 4),
        "iowait_percent": round(100 * delta[4] / total, 4),
        "steal_percent": round(100 * delta[7] / total, 4),
    }


def parse_network(text):
    result = {}
    indices = {"rx_bytes": 0, "rx_packets": 1, "rx_errors": 2, "rx_drops": 3,
               "tx_bytes": 8, "tx_packets": 9, "tx_errors": 10, "tx_drops": 11}
    for line in text.splitlines():
        if ":" not in line:
            continue
        name, counters = line.split(":", 1)
        fields = counters.split()
        result[name.strip()] = {key: int(fields[index]) for key, index in indices.items()}
    return result


def network_rates(previous, current, elapsed):
    if previous is None or elapsed <= 0:
        return None
    delta = {key: value - previous[key] for key, value in current.items()}
    if any(value < 0 for value in delta.values()):
        return None
    return {
        "rx_mbps": round(delta["rx_bytes"] * 8 / elapsed / 1_000_000, 6),
        "tx_mbps": round(delta["tx_bytes"] * 8 / elapsed / 1_000_000, 6),
        "rx_packets_per_second": round(delta["rx_packets"] / elapsed, 4),
        "tx_packets_per_second": round(delta["tx_packets"] / elapsed, 4),
        "rx_error_delta": delta["rx_errors"],
        "rx_drop_delta": delta["rx_drops"],
        "tx_error_delta": delta["tx_errors"],
        "tx_drop_delta": delta["tx_drops"],
    }


def memory_bytes(text):
    values = {}
    for line in text.splitlines():
        key, value = line.split(":", 1)
        fields = value.split()
        if len(fields) == 2 and fields[1] == "kB":
            values[key] = int(fields[0]) * 1024
    return {key: values.get(key) for key in
            ("MemTotal", "MemAvailable", "SwapTotal", "SwapFree")}


def read_pressure(proc):
    result = {}
    for resource in ("cpu", "memory", "io"):
        try:
            text = (proc / "pressure" / resource).read_text()
            result[resource] = {}
            for line in text.splitlines():
                kind, *fields = line.split()
                row = {}
                for field in fields:
                    key, value = field.split("=")
                    row["total_stall_us" if key == "total" else key] = (
                        int(value) if key == "total" else float(value))
                result[resource][kind] = row
        except OSError as error:
            result[resource] = {"unavailable": str(error)}
    return result


def snapshot(proc, interfaces):
    net = parse_network((proc / "net/dev").read_text())
    missing = set(interfaces) - net.keys()
    if missing:
        raise ValueError("requested interfaces unavailable: " + ", ".join(sorted(missing)))
    return {
        "cpu": parse_cpu((proc / "stat").read_text()),
        "network": {key: value for key, value in net.items()
                    if not interfaces or key in interfaces},
        "memory_bytes": memory_bytes((proc / "meminfo").read_text()),
        "pressure": read_pressure(proc),
        "load_average_1_5_15_min": [
            float(value) for value in (proc / "loadavg").read_text().split()[:3]],
        "monotonic": time.monotonic(),
    }


def emit(stream, record):
    stream.write(json.dumps(record, allow_nan=False) + "\n")
    stream.flush()


def collect(args, stream):
    proc = Path("/proc")
    previous = snapshot(proc, args.interface)
    emit(stream, {
        "kind": "metadata", "schema": 1, "kernel": platform.release(),
        "interval_seconds_requested": args.interval, "samples_requested": args.samples,
        "interfaces": list(previous["network"]),
        "logical_cpus_observed": len(previous["cpu"]) - 1,
        "scope": "Linux kernel resource counters; no Mumble quality or per-VM attribution",
        "notes": [
            "Null intervals mean counters reset, decreased, appeared, or did not advance.",
            "PSI avg10/avg60/avg300 are percentages; total_stall_us is cumulative.",
            "CPU busy excludes idle, iowait and steal; inspect steal separately.",
            "Do not sum bridge, tap, and physical NIC traffic; that double-counts packets.",
        ],
    })
    deadline = previous["monotonic"]
    written = 0
    try:
        for _ in range(args.samples):
            deadline += args.interval
            time.sleep(max(0, deadline - time.monotonic()))
            current = snapshot(proc, args.interface)
            elapsed = current["monotonic"] - previous["monotonic"]
            emit(stream, {
                "kind": "sample",
                "time_utc": datetime.now(timezone.utc).isoformat(),
                "elapsed_seconds": elapsed,
                "cpu": {key: cpu_usage(previous["cpu"].get(key), value)
                        for key, value in current["cpu"].items()},
                "network": {key: network_rates(previous["network"].get(key), value, elapsed)
                            for key, value in current["network"].items()},
                "memory_bytes": current["memory_bytes"],
                "pressure": current["pressure"],
                "load_average_1_5_15_min": current["load_average_1_5_15_min"],
            })
            written += 1
            previous = current
            # Avoid rapid catch-up samples after a delayed collection.
            deadline = max(deadline, current["monotonic"])
    except KeyboardInterrupt:
        emit(stream, {"kind": "end", "status": "interrupted", "samples_written": written})
        return 130
    emit(stream, {"kind": "end", "status": "complete", "samples_written": written})
    return 0


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--samples", type=positive_count, default=60)
    cli.add_argument("--interval", type=positive_interval, default=1.0)
    cli.add_argument("--interface", action="append", default=[],
                     help="Repeat to select interfaces; default reports every interface separately.")
    cli.add_argument("--output", type=Path, help="Create a new JSONL file; never overwrite.")
    args = cli.parse_args()
    if platform.system() != "Linux":
        cli.error("run on the Linux host or guest being measured")
    try:
        output = (args.output.open("x", encoding="utf-8") if args.output else
                  contextlib.nullcontext(sys.stdout))
        with output as stream:
            return collect(args, stream)
    except (OSError, ValueError) as error:
        cli.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
