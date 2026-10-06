#!/usr/bin/env python3
"""Linux/container-only receive-buffer experiment; not a production launcher.

Create one UDP socket with a larger buffer, drop to UID/GID 1000, then let a
test-only bind shim pass that socket to the unchanged pilot server. No host
sysctl is changed and the server runs with no effective capabilities.
"""

import argparse
import json
import os
from pathlib import Path
import socket


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--receive-buffer-bytes", type=int, required=True)
    args = cli.parse_args()
    if not Path("/.dockerenv").exists() or os.environ.get("PILOT_SOCKET_EXPERIMENT") != "1":
        cli.error("requires an explicitly designated disposable pilot container")
    if os.getuid() != 0 or not 131072 <= args.receive_buffer_bytes <= 4 * 1024 * 1024:
        cli.error("launcher requires root and a 128 KiB–4 MiB request")
    prepared = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Linux SO_RCVBUFFORCE bypasses rmem_max for this socket only.
    prepared.setsockopt(socket.SOL_SOCKET, 33, args.receive_buffer_bytes)
    actual = prepared.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    if actual != args.receive_buffer_bytes * 2:
        raise RuntimeError("unexpected Linux receive-buffer accounting")
    os.setgroups([])
    os.setgid(1000)
    os.setuid(1000)
    status = dict(line.split(":", 1) for line in Path("/proc/self/status").read_text().splitlines())
    if any(int(status[field].strip(), 16) for field in ("CapEff", "CapPrm", "CapAmb")):
        raise RuntimeError("capabilities were not cleared before starting the server")
    metadata = {"requested_receive_buffer_bytes": args.receive_buffer_bytes,
                "effective_receive_buffer_bytes": actual, "server_uid": os.getuid(),
                "server_gid": os.getgid(), "effective_capabilities": 0,
                "host_sysctls_changed": False}
    with Path("/runtime/socket-launcher.json").open("x") as stream:
        json.dump(metadata, stream, indent=2)
        stream.write("\n")
    prepared.set_inheritable(True)
    environment = os.environ.copy()
    environment["PILOT_PREPARED_UDP_FD"] = str(prepared.fileno())
    environment["LD_PRELOAD"] = "/build/libmumble_pilot_socket.so"
    os.execve("/build/mumble-server", ["/build/mumble-server", "--foreground", "--ini",
                                      "/runtime/server.ini"], environment)


if __name__ == "__main__":
    main()
