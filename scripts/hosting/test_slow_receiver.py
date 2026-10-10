#!/usr/bin/env python3
"""Start an owned loopback server and verify a stalled TLS receiver is isolated.

Requires Python 3, OpenSSL CLI and ps (macOS/Linux). Creates a NEW private
profile at --profile; stops only the subprocess it starts. Transfers 40 MiB
of opaque valid-length FileData over ~27 seconds, samples server RSS and
checks every healthy recipient payload/index plus control ping responses.
Use --expect-disconnect for the corrected server, omit for a baseline record.
This does not measure native devices, cryptographic file engines, VM density,
queued cross-thread media callbacks or audio/video fairness under saturation.
"""

import argparse, asyncio, hashlib, json, os, socket, ssl, struct, subprocess, time
from pathlib import Path
from relay_probe import blob, integer, read_varint


def values(data):
    result = {}
    offset = 0
    while offset < len(data):
        tag, offset = read_varint(data, offset)
        field, wire = tag >> 3, tag & 7
        if not field:
            raise ValueError("Invalid protobuf field")
        if wire == 0:
            value, offset = read_varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                length, offset = read_varint(data, offset)
            else:
                length = 8 if wire == 1 else 4
            if length > len(data) - offset:
                raise ValueError("Truncated protobuf field")
            value = data[offset : offset + length]
            offset += length
        else:
            raise ValueError("Unsupported protobuf wire type")
        result.setdefault(field, []).append(value)
    return result


def one(data, field, default=None):
    return data.get(field, [default])[-1]


class Peer:
    def __init__(self, args, name, password="", certificate=None):
        self.args, self.name, self.password, self.certificate = (
            args,
            name,
            password,
            certificate,
        )
        self.channels, self.users = {}, {}
        self.writer = None
        self.session = None

    async def send(self, kind, payload):
        self.writer.write(struct.pack(">HI", kind, len(payload)) + payload)
        await self.writer.drain()

    async def packet(self):
        kind, length = struct.unpack(
            ">HI", await asyncio.wait_for(self.reader.readexactly(6), 10)
        )
        if length > 1024 * 1024:
            raise ValueError("Oversized server message")
        raw = await asyncio.wait_for(self.reader.readexactly(length), 10)
        data = values(raw)
        if kind == 7 and one(data, 1) is not None:
            self.channels.setdefault(one(data, 1), {}).update(data)
        elif kind == 9 and one(data, 1) is not None:
            self.users.setdefault(one(data, 1), {}).update(data)
        return kind, data

    async def wait(self, kind, predicate=lambda data: True):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            received, data = await self.packet()
            if received == kind and predicate(data):
                return data
            if received == 4:
                raise RuntimeError("Server rejected fixture login")
            if received == 12 and kind != 12:
                raise RuntimeError("Server refused fixture administration request")
        raise TimeoutError("Fixture response deadline expired")

    async def connect(self):
        context = ssl.create_default_context(cafile=str(self.args.ca_file))
        if self.certificate:
            context.load_cert_chain(
                self.args.secrets / (self.certificate + ".crt"),
                self.args.secrets / (self.certificate + ".key"),
            )
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(
                self.args.host,
                self.args.port,
                ssl=context,
                server_hostname=self.args.server_name,
            ),
            10,
        )
        await self.send(0, integer(1, 0x10700) + blob(2, b"output-queue-probe"))
        await self.send(
            2,
            blob(1, self.name.encode())
            + integer(5, 1)
            + (blob(2, self.password.encode()) if self.password else b""),
        )
        self.session = one(await self.wait(5), 1)
        return self

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (OSError, ssl.SSLError):
                pass


async def scenario(args, process):
    peers = []
    received = []
    latencies = []
    rss = []
    pending = {}
    removed = []
    running = True
    payload = b"Q" * (64 * 1024)
    count = 640
    transfer = bytes.fromhex("a17120d365c0439cb0cec09bb34e34d8")

    async def connect(name):
        peer = Peer(args, name)
        peers.append(peer)
        await peer.connect()
        await peer.send(9, integer(25, 1))
        return peer

    async def consume(peer, collect=False):
        try:
            while True:
                kind, data = await peer.packet()
                if kind == 28 and collect:
                    assert one(data, 2) == transfer and one(data, 5) == payload
                    assert one(data, 1) == sender.session
                    received.append(one(data, 3))
                if kind == 8 and collect:
                    removed.append(one(data, 1))
                if kind == 3 and one(data, 1) in pending:
                    latencies.append(time.monotonic() - pending.pop(one(data, 1)))
        except TimeoutError:
            if collect:
                raise
            return "idle"
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            return "closed"

    async def monitor():
        serial = 1
        while running:
            result = await asyncio.create_subprocess_exec(
                "ps",
                "-o",
                "rss=",
                "-p",
                str(process.pid),
                stdout=asyncio.subprocess.PIPE,
            )
            output, _ = await result.communicate()
            if output.strip():
                rss.append(int(output.strip()) * 1024)
            pending[serial] = time.monotonic()
            await healthy.send(3, integer(1, serial))
            serial += 1
            await asyncio.sleep(0.2)

    tasks = []
    try:
        sender = await connect("probe-sender")
        healthy = await connect("probe-healthy")
        slow = await connect("probe-slow")
        # An observed capability echo is a barrier before the first chunk.
        await healthy.wait(
            9, lambda state: one(state, 1) == slow.session and one(state, 25) == 1
        )
        assert one(healthy.users[sender.session], 25) == 1
        slow.writer.transport.pause_reading()
        tasks = [
            asyncio.create_task(consume(healthy, True)),
            asyncio.create_task(consume(sender)),
            asyncio.create_task(monitor()),
        ]
        await asyncio.sleep(0.6)
        initial = rss[-1]
        started = time.monotonic()
        for index in range(count):
            await sender.send(
                28,
                blob(2, transfer)
                + integer(3, index)
                + integer(4, count)
                + blob(5, payload),
            )
            await asyncio.sleep(0.04)
        deadline = time.monotonic() + 5
        while len(received) < count and time.monotonic() < deadline:
            await asyncio.sleep(0.05)
        assert received == list(
            range(count)
        ), f"Healthy receiver got {len(received)}/{count} chunks"
        await asyncio.sleep(0.5)
        paused = rss[-1]
        running = False
        await tasks[-1]
        slow.writer.transport.resume_reading()
        slow_count = 0
        slow_closed = False
        try:
            while True:
                kind, data = await asyncio.wait_for(slow.packet(), 3)
                if kind == 28:
                    assert (
                        one(data, 2) == transfer
                        and one(data, 5) == payload
                        and one(data, 3) == slow_count
                    )
                    slow_count += 1
                    if slow_count == count:
                        break
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            slow_closed = True
        except (TimeoutError, asyncio.TimeoutError):
            pass
        assert latencies and not pending, "Control ping responses missing"
        assert not tasks[0].done(), "Healthy receiver disconnected"
        if args.expect_disconnect:
            assert (
                slow_closed and slow_count < count
            ), "Stalled receiver was not disconnected"
            assert (
                slow.session in removed
            ), "Healthy receiver did not observe stalled user removal"
        for task in tasks[:-1]:
            if task.done():
                task.result()
        result = {
            "schema": 1,
            "kind": "loopback_tls_slow_receiver",
            "source_revision": args.revision,
            "bytes_sent": count * len(payload),
            "chunk_bytes": len(payload),
            "healthy_chunks": len(received),
            "healthy_payload_sha256": hashlib.sha256(payload * count).hexdigest(),
            "slow_chunks_after_resume": slow_count,
            "slow_closed": slow_closed,
            "healthy_observed_slow_removal": slow.session in removed,
            "expected_disconnect_asserted": args.expect_disconnect,
            "rss_initial_bytes": initial,
            "rss_paused_bytes": paused,
            "rss_peak_bytes": max(rss),
            "rss_growth_bytes": paused - initial,
            "healthy_ping_count": len(latencies),
            "healthy_ping_max_seconds": max(latencies),
            "duration_seconds": time.monotonic() - started,
            "filebandwidth_bits_per_second": 16000000,
            "aggregate_bits_per_second": 64000000,
            "limits": "Opaque valid-length FileData relay, not a cryptographic file engine or native media test",
        }
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
    finally:
        running = False
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for peer in peers:
            peer.writer.transport.abort() if peer.writer else None


async def main(args):
    args.server = args.server.resolve()
    args.profile = args.profile.resolve()
    args.output = args.output.resolve()
    args.profile.mkdir(mode=0o700, parents=True, exist_ok=False)
    cert = args.profile / "server.crt"
    key = args.profile / "server.key"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost",
            "-keyout",
            str(key),
            "-out",
            str(cert),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    os.chmod(key, 0o600)
    with socket.socket() as allocator:
        allocator.bind(("127.0.0.1", 0))
        port = allocator.getsockname()[1]
    ini = args.profile / "server.ini"
    ini.write_text(
        f"host=127.0.0.1\nport={port}\ndatabase={args.profile}/state.sqlite\nsslCert={cert}\nsslKey={key}\nusers=4\ntimeout=60\nautobanAttempts=0\nfilebandwidth=16000000\nfilebandwidthaggregate=64000000\nmaxfilesize=67108864\n"
    )
    args.host = "127.0.0.1"
    args.port = port
    args.ca_file = cert
    args.server_name = "localhost"
    with (args.profile / "server.log").open("wb") as log:
        process = subprocess.Popen(
            [str(args.server), "--foreground", "--ini", str(ini)],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("Private server exited")
                try:
                    reader, writer = await asyncio.open_connection(args.host, port)
                    writer.close()
                    await writer.wait_closed()
                    break
                except OSError:
                    await asyncio.sleep(0.05)
            else:
                raise TimeoutError("Private server not listening")
            await asyncio.wait_for(scenario(args, process), 45)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--expect-disconnect", action="store_true")
    asyncio.run(main(parser.parse_args()))
