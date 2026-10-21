"""Shared loopback-server fixture helpers for the compatibility scripts.

Keeps server boot, readiness and shutdown identical across run_interop.py and
check_database.py, and holds the TLS observer both scripts use.
"""
import json
import queue
import socket
import ssl
import struct
import subprocess
import threading
import time


def wait(predicate, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.15)
    raise AssertionError("Timed out waiting for " + repr(predicate))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def server_command(server, ini, legacy, *extra):
    """Official 1.5 servers use the older single-dash flags and expect -ini before -supw."""
    return [str(server), "-ini" if legacy else "--ini", str(ini), *extra]


def start_server(server, ini, legacy, log):
    return subprocess.Popen([str(server), "-fg" if legacy else "--foreground",
                             "-ini" if legacy else "--ini", str(ini)], stdout=log, stderr=log)


def wait_for_server(process, port, seconds=20):
    def ready():
        try:
            with socket.create_connection(("127.0.0.1", port), .2):
                return True
        except OSError:
            assert process.poll() is None, f"Server exited at startup with {process.returncode}"
            return False
    wait(ready, seconds)


def stop_process(process, result=None, timeout_key=None, seconds=10):
    """SIGTERM, then SIGKILL if the process hangs. Never raises, so callers' cleanup still runs."""
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(seconds)
    except subprocess.TimeoutExpired:
        if result is not None and timeout_key:
            result[timeout_key] = True
        process.kill()
        try:
            process.wait(seconds)
        except subprocess.TimeoutExpired:
            if result is not None and timeout_key:
                result[timeout_key + "_unkillable"] = True
    return process.returncode


class Peer:
    def __init__(self, port, proto, log):
        self.proto, self.log = proto, log
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.check_hostname = False
        # Private loopback fixture with an ephemeral, self-signed certificate.
        context.verify_mode = ssl.CERT_NONE
        self.sock = context.wrap_socket(socket.create_connection(("127.0.0.1", port)), server_hostname="localhost")
        self.sock.settimeout(.1)
        self.outgoing = queue.Queue()
        self.stop = False
        self.session = None
        self.users, self.channels, self.texts, self.errors = {}, {}, [], []
        self.received = 0
        self.thread = threading.Thread(target=self.collect)
        self.thread.start()
        self.send(0, proto.Version(version_v1=0x10700, version_v2=(1 << 48) | (7 << 32), release="interop-observer"))
        self.send(2, proto.Authenticate(username="SuperUser", password="loopback-fixture", opus=True))
        def synchronized():
            assert not self.errors, self.errors
            return self.session
        try:
            wait(synchronized)
        except Exception:
            self.close()
            raise

    def send(self, kind, msg):
        payload = msg.SerializeToString()
        self.outgoing.put(struct.pack(">HI", kind, len(payload)) + payload)

    def collect(self):
        buffer = b""
        last_ping = 0
        try:
            while not self.stop:
                if self.session and time.monotonic() - last_ping > 3:
                    self.send(3, self.proto.Ping(timestamp=time.monotonic_ns()))
                    last_ping = time.monotonic()
                while not self.outgoing.empty():
                    self.sock.sendall(self.outgoing.get_nowait())
                try:
                    data = self.sock.recv(65536)
                    if not data:
                        if not self.stop:
                            self.errors.append("Observer disconnected")
                        return
                    buffer += data
                except socket.timeout:
                    continue
                while len(buffer) >= 6:
                    kind, size = struct.unpack(">HI", buffer[:6])
                    if len(buffer) < 6 + size:
                        break
                    payload, buffer = buffer[6:6 + size], buffer[6 + size:]
                    self.received += 1
                    if kind == 5:
                        self.session = self.proto.ServerSync.FromString(payload).session
                    elif kind == 7:
                        msg = self.proto.ChannelState.FromString(payload)
                        old = self.channels.get(msg.channel_id, self.proto.ChannelState())
                        old.MergeFrom(msg)
                        self.channels[msg.channel_id] = old
                    elif kind == 9:
                        msg = self.proto.UserState.FromString(payload)
                        old = self.users.get(msg.session, self.proto.UserState())
                        old.MergeFrom(msg)
                        self.users[msg.session] = old
                    elif kind == 11:
                        self.texts.append(self.proto.TextMessage.FromString(payload))
                    elif kind in (4, 8):
                        self.errors.append(f"Unexpected reject/remove: {kind}")
                    self.log.write(json.dumps({"time": time.monotonic(), "type": kind, "bytes": size}) + "\n")
                    self.log.flush()
        except Exception as error:
            if not self.stop:
                self.errors.append(str(error))

    def close(self):
        self.stop = True
        self.thread.join(3)
        self.sock.close()
