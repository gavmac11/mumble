#!/usr/bin/env python3
"""Bounded authenticated media relay probe using synthetic or H.264 payloads.

Supports TLS UDPTunnel and encrypted UDP using this repository's OCB2 code.
Checks fan-out and observed relay delay, not audible quality or VM density.
Only run against a test server you control. The server certificate is verified.
"""

import argparse
import asyncio
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import ssl
import struct
import time
import uuid

from native_crypto import NativeCrypto
from native_pacer import NativePacer
from opus_fixture import read_packets as opus_packets


def varint(value):
    if value < 0:
        raise ValueError("negative varint")
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def integer(field, value):
    return varint(field << 3) + varint(value)


def blob(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value


def read_varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError("oversized protobuf varint")


def fields(data):
    result = {}
    offset = 0
    while offset < len(data):
        tag, offset = read_varint(data, offset)
        field, wire = tag >> 3, tag & 7
        if field == 0:
            raise ValueError("invalid protobuf field")
        if wire == 0:
            value, offset = read_varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                length, offset = read_varint(data, offset)
            else:
                length = 8 if wire == 1 else 4
            if offset + length > len(data):
                raise ValueError("truncated protobuf field")
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError("unsupported protobuf wire type")
        result[field] = value
    return result


def percentiles(values):
    if not values:
        return None
    ordered = sorted(values)
    return {name: round(ordered[min(len(ordered) - 1,
                                   max(0, math.ceil(len(ordered) * fraction) - 1))], 3)
            for name, fraction in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1))}


def annexb_frames(data):
    """Split an H.264 fixture at access-unit delimiters emitted by x264 aud=1."""
    if not data or len(data) > 32 * 1024 * 1024:
        raise ValueError("H.264 fixture must contain 1 byte to 32 MiB")
    starts = [match.start() for match in re.finditer(b"\x00\x00(?:\x00)?\x01", data)
              if match.end() < len(data) and data[match.end()] & 31 == 9]
    if not starts:
        raise ValueError("H.264 fixture requires an AUD before each frame")
    starts[0] = 0
    starts.append(len(data))
    return [data[left:right] for left, right in zip(starts, starts[1:])]


class DatagramReceiver(asyncio.DatagramProtocol):
    def __init__(self, client):
        self.client = client

    def datagram_received(self, data, address):
        try:
            plaintext = self.client.crypto.decrypt(data)
            if plaintext is None:
                self.client.errors.append("udp_authentication_or_replay_rejection")
                return
            self.client.receive_media(plaintext, "udp")
        except (ValueError, OSError) as error:
            self.client.errors.append(f"udp_decode_{type(error).__name__}")

    def error_received(self, error):
        self.client.errors.append(f"udp_socket_{type(error).__name__}")


def video_delivery_accounting(shared, received, clients):
    """Keep source loss visible while counting relay expectations only after submission."""
    offered = set(shared["offered"])
    accepted = shared["accepted"]
    submitted = set(shared["sent"])
    if not submitted <= accepted <= offered:
        raise ValueError("inconsistent video submission accounting")
    fanout = clients - 1
    return {
        "offered_fragments": len(offered),
        "accepted_fragments": len(accepted),
        "pacer_rejected_fragments": len(offered - accepted),
        "accepted_not_submitted_by_deadline": len(accepted - submitted),
        "sent_fragments": len(submitted),
        "offered_expected_deliveries": len(offered) * fanout,
        "expected_deliveries": len(submitted) * fanout,
        "undelivered_by_deadline": len(submitted) * fanout - received,
        "total_missing_deliveries": len(offered) * fanout - received,
    }


class Client:
    def __init__(self, name, shared):
        self.name = name
        self.shared = shared
        self.session = None
        self.ready = asyncio.get_running_loop().create_future()
        self.reader_task = None
        self.writer = None
        self.seen = set()
        self.received_bytes = 0
        self.latencies = []
        self.ready_latencies = []
        self.pings = []
        self.errors = []
        self.capture = False
        self.captured = {}
        self.crypto_setup = asyncio.get_running_loop().create_future()
        self.crypto = None
        self.udp = None
        self.udp_ready = asyncio.Event()
        self.udp_ping_sent = set()
        self.udp_pings = []
        self.media_received_by_transport = {"tcp": 0, "udp": 0}
        self.voice_seen = set()
        self.voice_latencies = []
        self.native_pacer = None
        self.pacer_task = None
        self.pacer_wakeup = asyncio.Event()
        self.pacer_rejected_frames = 0

    async def send(self, kind, payload):
        self.writer.write(struct.pack(">HI", kind, len(payload)) + payload)
        await self.writer.drain()

    async def connect(self, args, context):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(args.host, args.port, ssl=context,
                                    server_hostname=args.server_name or args.host), 15)
        self.reader_task = asyncio.create_task(self.receive())
        version = integer(1, 0x10700) + integer(5, (1 << 48) | (7 << 32))
        await self.send(0, version + blob(2, b"hosting-relay-probe"))
        await self.send(2, blob(1, self.name.encode()) + integer(5, 1))
        await asyncio.wait_for(self.ready, 15)
        await self.send(9, integer(1, self.session) + integer(24, 1))
        if args.transport == "udp":
            key, client_nonce, server_nonce = await asyncio.wait_for(self.crypto_setup, 5)
            self.crypto = NativeCrypto(args.crypto_library, key, client_nonce, server_nonce)
            # Use the established TCP peer address so both transports identify the same server.
            peer = self.writer.get_extra_info("peername")
            self.udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: DatagramReceiver(self), remote_addr=(peer[0], args.port))
            for _ in range(3):
                self.send_udp_ping()
                try:
                    await asyncio.wait_for(self.udp_ready.wait(), 1)
                    break
                except asyncio.TimeoutError:
                    pass
            if not self.udp_ready.is_set():
                raise RuntimeError("encrypted UDP connectivity check failed")
        if args.native_pacer_library:
            self.native_pacer = NativePacer(args.native_pacer_library, int(args.video_send_cap_mbps * 1_000_000))
            self.pacer_task = asyncio.create_task(self.send_paced_packets())

    async def send_paced_packets(self):
        while True:
            packet = self.native_pacer.take(time.perf_counter_ns())
            if packet:
                video = fields(packet[1:])
                key = (self.session, video.get(5, 0), video.get(6, 0))
                expected = self.shared["offered"][key]
                self.shared["sent"][key] = (time.perf_counter(), *expected[1:])
                self.shared["payload_sent"] += len(video[8])
                await self.send_media(packet)
                continue
            delay = self.native_pacer.delay_ns(time.perf_counter_ns())
            if delay is None:
                await self.pacer_wakeup.wait()
                self.pacer_wakeup.clear()
            else:
                await asyncio.sleep(delay / 1_000_000_000)

    def send_udp_ping(self):
        timestamp = time.perf_counter_ns()
        self.udp_ping_sent.add(timestamp)
        self.udp.sendto(self.crypto.encrypt(bytes([1]) + integer(1, timestamp)))

    async def send_media(self, packet):
        if self.udp:
            self.udp.sendto(self.crypto.encrypt(packet))
        else:
            await self.send(1, packet)

    def receive_media(self, payload, transport):
        if not payload:
            raise ValueError("empty media packet")
        if payload[0] == 1:
            timestamp = fields(payload[1:]).get(1)
            if timestamp in self.udp_ping_sent:
                self.udp_ping_sent.remove(timestamp)
                self.udp_pings.append((time.perf_counter_ns() - timestamp) / 1_000_000)
                self.udp_ready.set()
            return
        if payload[0] == 0:
            voice = fields(payload[1:])
            key = (voice.get(3), voice.get(4, 0))
            expected = self.shared["voice_sent"].get(key)
            if expected is None or key[0] == self.session or key in self.voice_seen:
                self.errors.append("unexpected_or_duplicate_voice")
                return
            if hashlib.sha256(voice.get(5, b"")).digest() != expected[1]:
                self.errors.append({"type": "voice_payload_mismatch", "frame": key[1],
                                    "actual_bytes": len(voice.get(5, b"")),
                                    "actual_sha256": hashlib.sha256(voice.get(5, b"")).hexdigest(),
                                    "expected_sha256": expected[1].hex()})
                return
            self.media_received_by_transport[transport] += 1
            self.voice_seen.add(key)
            self.voice_latencies.append((time.perf_counter() - expected[0]) * 1000)
            return
        if payload[0] != 2:
            self.errors.append("unexpected_media_type")
            return
        video = fields(payload[1:])
        key = (video.get(1), video.get(5, 0), video.get(6, 0))
        expected = self.shared["sent"].get(key)
        if expected is None:
            self.errors.append("unexpected_video")
            return
        if key[0] == self.session or key in self.seen:
            self.errors.append("self_echo_or_duplicate")
            return
        encoded = video.get(8, b"")
        if hashlib.sha256(encoded).digest() != expected[1]:
            self.errors.append("payload_mismatch")
            return
        if (video.get(7, 0), video.get(3, 0), video.get(4, 0)) != expected[2:]:
            self.errors.append("video_metadata_mismatch")
            return
        self.media_received_by_transport[transport] += 1
        self.seen.add(key)
        self.received_bytes += len(encoded)
        self.latencies.append((time.perf_counter() - expected[0]) * 1000)
        self.ready_latencies.append((time.perf_counter() - self.shared["frame_ready"][key[:2]]) * 1000)
        if self.capture:
            count = video.get(7, 0)
            record = self.captured.setdefault(key[:2], [None] * count)
            if not 0 <= key[2] < len(record) or count != len(record):
                self.errors.append("invalid_capture_fragment")
            else:
                record[key[2]] = encoded

    async def receive(self):
        try:
            while True:
                kind, length = struct.unpack(">HI", await self.reader.readexactly(6))
                if length > 1_048_576:
                    raise ValueError("oversized control message")
                payload = await self.reader.readexactly(length)
                if kind == 1:
                    self.receive_media(payload, "tcp")
                elif kind == 15:
                    setup = fields(payload)
                    if not self.crypto_setup.done() and all(len(setup.get(i, b"")) == 16 for i in (1, 2, 3)):
                        self.crypto_setup.set_result(tuple(setup[i] for i in (1, 2, 3)))
                    elif self.crypto:
                        self.errors.append("crypto_resync_not_supported_by_probe")
                elif kind == 5:
                    self.session = fields(payload)[1]
                    if not self.ready.done():
                        self.ready.set_result(self.session)
                elif kind == 3:
                    timestamp = fields(payload).get(1)
                    if timestamp:
                        self.pings.append((time.perf_counter_ns() - timestamp) / 1_000_000)
                elif kind in (4, 8, 12):
                    details = fields(payload)
                    reason = details.get(2, details.get(3, b""))
                    message = f"server control error {kind}: {reason!r}"
                    if not self.ready.done():
                        self.ready.set_exception(RuntimeError(message))
                    self.errors.append(message)
        except (asyncio.IncompleteReadError, OSError, ValueError) as error:
            if not self.ready.done():
                self.ready.set_exception(error)
            self.errors.append(type(error).__name__)

    async def stream(self, args, start):
        frame_size = int(args.sender_mbps * 1_000_000 / 8 / args.fps)
        next_packet = start
        for frame in range(int(args.seconds * args.fps)):
            await asyncio.sleep(max(0, start + frame / args.fps - time.perf_counter()))
            self.shared["frame_ready"][(self.session, frame)] = start + frame / args.fps
            encoded_frame = (args.video_frames[frame % len(args.video_frames)]
                             if args.video_frames else b"P" * frame_size)
            keyframe = bool(args.video_frames and any(
                match.end() < len(encoded_frame) and encoded_frame[match.end()] & 31 == 5
                for match in re.finditer(b"\x00\x00(?:\x00)?\x01", encoded_frame)))
            fragments = [encoded_frame[offset:offset + 900]
                         for offset in range(0, len(encoded_frame), 900)]
            frame_packets = []
            for index, chunk in enumerate(fragments):
                video = (integer(1, self.session) + integer(3, args.width) + integer(4, args.height) + integer(5, frame) +
                         integer(6, index) + integer(7, len(fragments)) + blob(8, chunk))
                if keyframe and index == 0:
                    video += integer(9, 1)
                packet = bytes([2]) + video
                if len(packet) > 1024:
                    raise ValueError("video packet exceeds Mumble packet limit")
                if args.video_send_cap_mbps and not self.native_pacer:
                    # Account for IPv4 + UDP + crypto overhead as well as the protobuf.
                    interval = (len(packet) + 32) * 8 / (args.video_send_cap_mbps * 1_000_000)
                    await asyncio.sleep(max(0, next_packet - time.perf_counter()))
                    # Permit at most one packet interval of scheduler catch-up.
                    next_packet = max(next_packet + interval, time.perf_counter())
                key = (self.session, frame, index)
                self.shared["offered"][key] = (
                    time.perf_counter(), hashlib.sha256(chunk).digest(),
                    len(fragments), args.width, args.height)
                self.shared["encoded_payload_offered"] += len(chunk)
                if self.native_pacer:
                    frame_packets.append(packet)
                    continue
                self.shared["accepted"].add(key)
                self.shared["sent"][key] = self.shared["offered"][key]
                self.shared["payload_sent"] += len(chunk)
                await self.send_media(packet)
            if self.native_pacer:
                if not self.native_pacer.enqueue(frame_packets, keyframe, time.perf_counter_ns()):
                    self.pacer_rejected_frames += 1
                else:
                    self.shared["accepted"].update((self.session, frame, index) for index in range(len(fragments)))
                self.pacer_wakeup.set()
        await asyncio.sleep(max(0, start + args.seconds - time.perf_counter()))

    async def close(self):
        if self.pacer_task:
            self.pacer_task.cancel()
            await asyncio.gather(self.pacer_task, return_exceptions=True)
        if self.native_pacer:
            self.native_pacer.close()
        if self.udp:
            self.udp.close()
            await asyncio.sleep(0)
        if self.crypto:
            self.crypto.close()
        if self.reader_task:
            self.reader_task.cancel()
            await asyncio.gather(self.reader_task, return_exceptions=True)
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (ConnectionError, ssl.SSLError):
                pass

    async def stream_voice(self, args, start):
        count = int(args.seconds * 50)
        for index in range(count):
            await asyncio.sleep(max(0, start + index * .02 - time.perf_counter()))
            encoded = args.voice_packets[index % len(args.voice_packets)]
            frame = index * 2  # Mumble frame numbers count 10 ms units.
            packet = bytes([0]) + integer(1, 0) + integer(4, frame) + blob(5, encoded)
            if index == count - 1:
                packet += integer(16, 1)
            self.shared["voice_sent"][(self.session, frame)] = (
                time.perf_counter(), hashlib.sha256(encoded).digest())
            self.shared["voice_payload_sent"] += len(encoded)
            await self.send_media(packet)


async def ping_clients(clients):
    while True:
        for client in clients:
            if client.udp:
                client.send_udp_ping()
        await asyncio.gather(*(client.send(3, integer(1, time.perf_counter_ns()))
                               for client in clients))
        await asyncio.sleep(.5)


async def measure_loop_lag(observations):
    while True:
        start = time.perf_counter()
        await asyncio.sleep(.02)
        observations.append(max(0, (time.perf_counter() - start - .02) * 1000))


async def run(args):
    context = ssl.create_default_context(cafile=str(args.ca_file) if args.ca_file else None)
    shared = {"offered": {}, "accepted": set(), "encoded_payload_offered": 0,
              "sent": {}, "payload_sent": 0, "voice_sent": {}, "voice_payload_sent": 0,
              "frame_ready": {}}
    run_id = uuid.uuid4().hex[:8]
    clients = [Client(f"pilot-{run_id}-{index}", shared) for index in range(args.clients)]
    clients[-1].capture = args.capture_directory is not None
    background = []
    loop_lag = []
    try:
        # Sequential TLS handshakes avoid confusing an authentication burst with media load.
        for client in clients:
            await client.connect(args, context)
        background = [asyncio.create_task(ping_clients(clients)),
                      asyncio.create_task(measure_loop_lag(loop_lag))]
        await asyncio.sleep(.25)
        started_at = datetime.now(timezone.utc).isoformat()
        start = time.perf_counter()
        cpu_start = time.process_time()
        await asyncio.wait_for(asyncio.gather(
            *(client.stream(args, start + (index / args.senders / args.fps
                                           if args.stagger_video_frames else 0))
              for index, client in enumerate(clients[:args.senders])),
            *(client.stream_voice(args, start) for client in clients[:args.voice_senders])),
            timeout=args.seconds + 15)
        offered_duration = time.perf_counter() - start
        offer_cpu = time.process_time() - cpu_start
        await asyncio.sleep(args.settle_seconds)
        for task in background:
            if task.done():
                task.result()
        for client in clients:
            if client.pacer_task and client.pacer_task.done():
                client.pacer_task.result()
        received = sum(len(client.seen) for client in clients)
        video_accounting = video_delivery_accounting(shared, received, args.clients)
        expected = video_accounting["expected_deliveries"]
        voice_expected = len(shared["voice_sent"]) * (args.clients - 1)
        voice_received = sum(len(client.voice_seen) for client in clients)
        errors = [error for client in clients for error in client.errors]
        captures = []
        if args.capture_directory:
            observer = clients[-1]
            for sender in clients[:args.senders]:
                if sender.session == observer.session:
                    continue
                output = args.capture_directory / f"sender-{sender.session}.h264"
                complete_frames = 0
                incomplete_frames = []
                with output.open("xb") as stream:
                    for frame in range(int(args.seconds * args.fps)):
                        fragments = observer.captured.get((sender.session, frame))
                        if fragments and all(chunk is not None for chunk in fragments):
                            stream.write(b"".join(fragments))
                            complete_frames += 1
                        else:
                            incomplete_frames.append(frame)
                captures.append({"file": output.name, "complete_frames": complete_frames,
                                 "expected_frames": int(args.seconds * args.fps),
                                 "incomplete_frames": incomplete_frames,
                                 "sha256": hashlib.sha256(output.read_bytes()).hexdigest()})
        return {
            "schema_version": 2,
            "scope": ("H.264 fixture; decode captured streams separately" if args.video_frames else
                      "synthetic video payload; no decoded media validation"),
            "transport": args.transport,
            "media_deliveries_by_transport": {
                mode: sum(client.media_received_by_transport[mode] for client in clients)
                for mode in ("tcp", "udp")},
            "crypto_library_sha256": (hashlib.sha256(args.crypto_library.read_bytes()).hexdigest()
                                      if args.transport == "udp" else None),
            "run_id": run_id,
            "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "fixture_sha256": (hashlib.sha256(args.h264_fixture.read_bytes()).hexdigest()
                               if args.h264_fixture else None),
            "voice_fixture_sha256": (hashlib.sha256(args.opus_fixture.read_bytes()).hexdigest()
                                     if args.opus_fixture else None),
            "start_time_utc": started_at,
            "end_time_utc": datetime.now(timezone.utc).isoformat(),
            "clients": args.clients, "senders": args.senders,
            "stagger_video_frames": args.stagger_video_frames,
            "video_send_cap_mbps": args.video_send_cap_mbps,
            "native_pacer_library_sha256": (hashlib.sha256(args.native_pacer_library.read_bytes()).hexdigest()
                                            if args.native_pacer_library else None),
            "native_pacer_rejected_frames": sum(client.pacer_rejected_frames for client in clients),
            "frames_per_second": args.fps, "width": args.width, "height": args.height,
            "requested_sender_payload_mbps": None if args.video_frames else args.sender_mbps,
            "requested_seconds": args.seconds,
            "actual_offer_seconds": round(offered_duration, 4),
            "generator_cpu_seconds_during_offer": round(offer_cpu, 4),
            "generator_cpu_percent_of_one_core": round(100 * offer_cpu / offered_duration, 2),
            "settle_seconds": args.settle_seconds,
            "offered_payload_mbps": round(shared["encoded_payload_offered"] * 8 /
                                           offered_duration / 1_000_000, 4),
            "submitted_payload_mbps": round(shared["payload_sent"] * 8 / offered_duration / 1_000_000, 4),
            **video_accounting,
            "observed_unique_deliveries": received,
            "integrity_errors": errors,
            "complete_fanout": (expected > 0 and video_accounting["total_missing_deliveries"] == 0 and not errors and
                                voice_received == voice_expected and
                                all(client.media_received_by_transport[args.transport] == len(client.seen) + len(client.voice_seen)
                                    for client in clients)),
            "voice": {
                "senders": args.voice_senders,
                "packet_duration_ms": 20 if args.voice_senders else None,
                "sent_packets": len(shared["voice_sent"]),
                "expected_deliveries": voice_expected,
                "observed_unique_deliveries": voice_received,
                "undelivered_by_deadline": voice_expected - voice_received,
                "offered_payload_mbps": round(shared["voice_payload_sent"] * 8 / offered_duration / 1_000_000, 4),
                "relay_delay_ms": percentiles([value for client in clients for value in client.voice_latencies]),
            },
            "relay_delay_ms": percentiles([value for client in clients
                                          for value in client.latencies]),
            "scheduled_frame_to_fragment_delivery_ms": percentiles(
                [value for client in clients for value in client.ready_latencies]),
            "control_ping_rtt_ms": percentiles([value for client in clients for value in client.pings]),
            "encrypted_udp_ping_rtt_ms": percentiles([value for client in clients for value in client.udp_pings]),
            "generator_event_loop_lag_ms": percentiles(loop_lag),
            "captures": captures,
        }
    finally:
        for task in background:
            task.cancel()
        await asyncio.gather(*background, return_exceptions=True)
        await asyncio.gather(*(client.close() for client in clients), return_exceptions=True)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--host", default="127.0.0.1")
    cli.add_argument("--port", type=int, default=64758)
    cli.add_argument("--ca-file", type=Path)
    cli.add_argument("--server-name", help="Expected TLS certificate hostname")
    cli.add_argument("--transport", choices=("tcp", "udp"), default="tcp")
    cli.add_argument("--crypto-library", type=Path, help="Native OCB2 bridge; required for UDP")
    cli.add_argument("--native-pacer-library", type=Path, help="Use the actual C++ client queue through the test bridge")
    cli.add_argument("--clients", type=int, default=10)
    cli.add_argument("--senders", type=int, default=1)
    cli.add_argument("--stagger-video-frames", action="store_true",
                     help="Spread sender start phases across one video frame interval")
    cli.add_argument("--video-send-cap-mbps", type=float, default=0,
                     help="Experimental per-sender packet pacing, including estimated overhead; 0 disables")
    cli.add_argument("--sender-mbps", type=float, default=.2)
    cli.add_argument("--seconds", type=float, default=3)
    cli.add_argument("--fps", type=int, default=30)
    cli.add_argument("--settle-seconds", type=float, default=2)
    cli.add_argument("--h264-fixture", type=Path, help="Annex-B stream with AUD per frame; overrides sender-mbps.")
    cli.add_argument("--opus-fixture", type=Path, help="Ogg Opus fixture, 20 ms packets, up to 48 kbps")
    cli.add_argument("--voice-senders", type=int, default=0)
    cli.add_argument("--capture-directory", type=Path, help="New directory for one observer's reassembled streams.")
    cli.add_argument("--width", type=int, default=1280)
    cli.add_argument("--height", type=int, default=720)
    args = cli.parse_args()
    if not 2 <= args.clients <= 10 or not 1 <= args.senders <= args.clients:
        cli.error("probe is bounded to 2–10 clients and 1–clients senders")
    if not math.isfinite(args.sender_mbps) or not .01 <= args.sender_mbps <= 2:
        cli.error("sender-mbps must be between .01 and 2")
    if not math.isfinite(args.video_send_cap_mbps) or (args.video_send_cap_mbps != 0 and not .1 <= args.video_send_cap_mbps <= 4):
        cli.error("video-send-cap-mbps must be 0 or between .1 and 4")
    if not math.isfinite(args.seconds) or not 1 <= args.seconds <= 30:
        cli.error("seconds must be between 1 and 30")
    if not math.isfinite(args.settle_seconds) or not 1 <= args.settle_seconds <= 10:
        cli.error("settle-seconds must be between 1 and 10")
    if not 1 <= args.fps <= 30 or not 1 <= args.port <= 65535:
        cli.error("fps must be 1–30 and port 1–65535")
    if args.width <= 0 or args.height <= 0:
        cli.error("frame dimensions must be positive and match the fixture")
    if args.capture_directory and not args.h264_fixture:
        cli.error("capture-directory requires an H.264 fixture")
    if args.transport == "udp" and not args.crypto_library:
        cli.error("UDP requires --crypto-library built from the pinned repository source")
    if args.native_pacer_library and (not args.video_send_cap_mbps or not args.h264_fixture):
        cli.error("native pacing requires a nonzero send cap and H.264 keyframe metadata")
    if not 0 <= args.voice_senders <= args.clients or (args.voice_senders and not args.opus_fixture):
        cli.error("voice-senders must be 0–clients and requires an Opus fixture when nonzero")
    try:
        args.video_frames = annexb_frames(args.h264_fixture.read_bytes()) if args.h264_fixture else None
        args.voice_packets = opus_packets(args.opus_fixture.read_bytes()) if args.opus_fixture else None
        if args.video_frames:
            if max(map(len, args.video_frames)) > 256 * 1024:
                cli.error("fixture frames must not exceed 256 KiB")
            offered = [args.video_frames[i % len(args.video_frames)]
                       for i in range(int(args.seconds * args.fps))]
            if sum(map(len, offered)) * 8 / args.seconds > 2_500_000:
                cli.error("fixture average payload must not exceed 2.5 Mbps per sender")
        if args.capture_directory:
            args.capture_directory.mkdir(parents=True, exist_ok=False)
        result = asyncio.run(run(args))
    except (OSError, ValueError, RuntimeError, asyncio.TimeoutError) as error:
        cli.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["complete_fanout"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
