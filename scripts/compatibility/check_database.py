#!/usr/bin/env python3
"""Open a stopped fixture's SQLite database and ini with another server binary.

Copies the database with SQLite's backup API. Requires the interop fixture's
SuperUser password and Interop channel, and never touches the original database.
"""
import argparse
import importlib
import json
import sqlite3
import sys
import time
from pathlib import Path

from fixture import Peer, free_port, start_server, stop_process, wait, wait_for_server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--legacy-server", action="store_true")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(args.fixture / "server.sqlite") as original:
        with sqlite3.connect(root / "server.sqlite") as copied:
            original.backup(copied)
    port = free_port()
    ini = (args.fixture / "server.ini").read_text()
    ini = "\n".join(f"database={root / 'server.sqlite'}" if line.startswith("database=") else
                    f"port={port}" if line.startswith("port=") else line for line in ini.splitlines()) + "\n"
    (root / "server.ini").write_text(ini)
    sys.path.insert(0, str(args.fixture / "generated"))
    proto = importlib.import_module("Mumble_pb2")
    log = (root / "server.log").open("w")
    process = start_server(args.server, root / "server.ini", args.legacy_server, log)
    peer = None
    result = {"pass": False, "fixture": str(args.fixture), "server": str(args.server)}
    try:
        wait_for_server(process, port)
        with (root / "observer.jsonl").open("w") as observer_log:
            peer = Peer(port, proto, observer_log)
            wait(lambda: any(c.name == "Interop" for c in list(peer.channels.values())))
            time.sleep(1)
            assert not peer.errors, peer.errors
            result["stored_authentication_and_channel"] = True
            peer.close()
            peer = None
        with sqlite3.connect(root / "server.sqlite") as database:
            result["integrity_check"] = database.execute("PRAGMA integrity_check").fetchone()[0]
        result["pass"] = result["integrity_check"] == "ok"
    except Exception as error:
        result["error"] = repr(error)
    finally:
        if peer:
            peer.close()
        stop_process(process, result, "server_shutdown_timeout")
        log.close()
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    return not result["pass"]


if __name__ == "__main__":
    raise SystemExit(main())
