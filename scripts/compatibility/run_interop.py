#!/usr/bin/env python3
"""Exercise a real Qt client against a loopback server under Xvfb.

Run inside dbus-run-session and xvfb-run. Requires Python pyatspi, Pillow,
protobuf, protoc, openssl and xdotool. Creates isolated config and databases.
The TLS observer uses SuperUser only in the disposable loopback fixture.
"""
import argparse
import importlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from PIL import ImageGrab

from fixture import Peer, free_port, server_command, start_server, stop_process, wait, wait_for_server


def walk(node, depth=0):
    if depth > 14:
        return
    yield node
    try:
        for child in node:
            yield from walk(child, depth + 1)
    except Exception:
        return


def find(predicate):
    context = GLib.MainContext.default()
    for _ in range(100):
        if not context.pending():
            break
        context.iteration(False)
    desktop = pyatspi.Registry.getDesktop(0)
    desktop.clearCache()
    for node in walk(desktop):
        try:
            if predicate(node):
                return node
        except Exception:
            continue
    return None


def click(node, twice=False):
    rect = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
    subprocess.run(["xdotool", "mousemove", str(rect.x + rect.width // 2),
                    str(rect.y + rect.height // 2), "click", "--repeat",
                    "2" if twice else "1", "--delay", "100", "1"], check=True)


def main():
    global dbus, pyatspi, GLib
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=125)
    parser.add_argument("--legacy-server", action="store_true")
    parser.add_argument("--fork-client", action="store_true")
    parser.add_argument("--fork-server", action="store_true")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    os.environ["XDG_RUNTIME_DIR"] = str(runtime)
    # Each private session bus must launch its accessibility bridge on a unique
    # runtime socket, including when multiple Xvfb fixtures run concurrently.
    subprocess.run(["dbus-update-activation-environment", "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR"], check=True)
    import dbus
    import pyatspi
    from gi.repository import GLib
    session_bus = dbus.SessionBus()
    generated = root / "generated"
    generated.mkdir(exist_ok=True)
    source = Path(__file__).resolve().parents[2]
    subprocess.run(["protoc", f"--proto_path={source / 'src'}", f"--python_out={generated}", "Mumble.proto"], check=True)
    sys.path.insert(0, str(generated))
    proto = importlib.import_module("Mumble_pb2")
    port = free_port()
    ini = root / "server.ini"
    ini.write_text(f"host=127.0.0.1\nport={port}\ndatabase={root / 'server.sqlite'}\nbonjour=false\n"
                   "autobanAttempts=0\nwelcometext=Compatibility loopback fixture\n")
    # Boot once so the virtual server and its SuperUser row exist before setting a password.
    with (root / "server-initialize.log").open("w") as initialize_log:
        initialize = start_server(args.server, ini, args.legacy_server, initialize_log)
        try:
            wait_for_server(initialize, port)
        finally:
            stop_process(initialize)
    with (root / "server-setup.log").open("w") as setup_log:
        subprocess.run(server_command(args.server, ini, args.legacy_server,
                                      "-supw" if args.legacy_server else "--set-su-pw", "loopback-fixture"),
                       stdout=setup_log, stderr=subprocess.STDOUT, check=True)
    server_log = (root / "server.log").open("w")
    client_log = (root / "client.log").open("w")
    observer_log = (root / "observer.jsonl").open("w")
    server = start_server(args.server, ini, args.legacy_server, server_log)
    client = peer = None
    result = {"pass": False, "client": str(args.client), "server": str(args.server)}
    try:
        wait_for_server(server, port)
        peer = Peer(port, proto, observer_log)
        peer.send(7, proto.ChannelState(parent=0, name="Interop"))
        channel = wait(lambda: next((c for c in list(peer.channels.values()) if c.name == "Interop"), None))
        (root / "client.sqlite").touch()
        config = root / "settings.json"
        config.write_text(json.dumps({"settings_version": 1, "misc": {
            "audio_wizard_has_been_shown": True, "viewed_server_ping_consent_message": True,
            "database_location": str(root / "client.sqlite"), "file_transfer_enabled": args.fork_client},
            "ui": {"send_usage_statistics": False},
            "update": {"check_for_updates": False, "check_for_plugin_updates": False},
            "audio": {"mute": True, "deaf": True}}))
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(root / "key.pem"),
                        "-out", str(root / "cert.pem"), "-days", "1", "-subj", "/CN=InteropFixture"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        subprocess.run(["openssl", "pkcs12", "-export", "-inkey", str(root / "key.pem"), "-in", str(root / "cert.pem"),
                        "-out", str(root / "MumbleAutomaticCertificateBackup.p12"), "-passout", "pass:"], check=True)
        env = {**os.environ, "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1", "QT_ACCESSIBILITY": "1",
               "XDG_CONFIG_HOME": str(root / "config"), "XDG_DATA_HOME": str(root / "data")}
        env.pop("QT_QPA_PLATFORM", None)
        client = subprocess.Popen([str(args.client), "--multiple", "--config", str(config),
                                   "--default-certificate-dir", str(root),
                                   f"mumble://GuiInterop@127.0.0.1:{port}/?version=1.5.0"],
                                  stdout=client_log, stderr=client_log, env=env)
        time.sleep(2)
        # Trust only this newly created loopback server's self-signed certificate.
        def connected():
            assert client.poll() is None, "Client exited at startup"
            user = next((u for u in list(peer.users.values()) if u.name == "GuiInterop"), None)
            if user:
                return user
            button = find(lambda n: n.getRoleName() == "push button" and n.name == "Yes"
                          and n.getState().contains(pyatspi.STATE_SHOWING))
            if button:
                click(button)
            return None
        user = wait(connected, 45)
        started = time.monotonic()
        time.sleep(1)
        # File sharing is on for every fork client. Only a relay-capable (fork) server may
        # trigger the one-time identity setup; official servers must not.
        prompt_expected = args.fork_client and args.fork_server
        def identity_dialog():
            return find(lambda n: n.getRoleName() == "dialog" and n.name == "File-transfer identity")
        try:
            identity_prompt = wait(identity_dialog, 10) if prompt_expected else identity_dialog()
        except AssertionError:
            identity_prompt = None
        result["identity_prompt_shown"] = bool(identity_prompt)
        assert bool(identity_prompt) == prompt_expected, ("identity prompt", bool(identity_prompt), prompt_expected)
        if identity_prompt:
            cancel = next(n for n in walk(identity_prompt) if n.getRoleName() == "push button" and n.name == "Cancel")
            click(cancel)
        wait(lambda: find(lambda n: n.getRoleName() == "table cell" and "GuiInterop" in n.name))
        wait(lambda: find(lambda n: n.getRoleName() == "table cell" and "SuperUser" in n.name))
        result["user_list"] = True
        # Use Mumble's supported desktop RPC: Qt 6.4's accessible cell rectangles
        # can point at a different row after the model inserts users/channels.
        desktop_rpc = dbus.Interface(dbus.SessionBus().get_object("net.sourceforge.mumble.mumble", "/"), "net.sourceforge.mumble.Mumble")
        desktop_rpc.openUrl(f"mumble://GuiInterop@127.0.0.1:{port}/Interop?version=1.5.0")
        wait(lambda: peer.users[user.session].channel_id == channel.channel_id)
        result["join_channel"] = True
        peer.send(9, proto.UserState(session=peer.session, channel_id=channel.channel_id))
        wait(lambda: peer.users[peer.session].channel_id == channel.channel_id)
        chat = wait(lambda: find(lambda n: n.name == "Enter chat message" and n.getRoleName() == "text"))
        chat.queryEditableText().setTextContents("gui-to-observer compatibility text")
        chat.queryComponent().grabFocus()
        subprocess.run(["xdotool", "key", "Return"], check=True)
        wait(lambda: any("gui-to-observer" in m.message for m in list(peer.texts)))
        peer.send(11, proto.TextMessage(session=[user.session], message="observer-to-gui compatibility text"))
        def received_text(needle="observer-to-gui compatibility text"):
            def has_text(n):
                try:
                    return needle in n.queryText().getText(0, -1)
                except Exception:
                    return False
            return find(has_text)
        wait(received_text)
        result["text_both_directions"] = True
        peer.send(9, proto.UserState(session=user.session, channel_id=0))
        wait(lambda: peer.users[user.session].channel_id == 0)
        desktop_rpc.openUrl(f"mumble://GuiInterop@127.0.0.1:{port}/Interop?version=1.5.0")
        wait(lambda: peer.users[user.session].channel_id == channel.channel_id)
        result["channel_moves"] = True
        if args.fork_client:
            expected = args.fork_server
            for name in ("Share video…", "Send a file to the channel"):
                control = wait(lambda: find(lambda n: n.name == name and n.getRoleName() in ("menu item", "push button")))
                enabled = control.getState().contains(pyatspi.STATE_ENABLED)
                assert enabled == expected, (name, enabled, expected)
            result["extension_controls_enabled"] = expected
        ImageGrab.grab().save(root / "connected.png")
        while time.monotonic() - started < args.seconds:
            assert client.poll() is None, "Client exited during soak"
            assert server.poll() is None, "Server exited during soak"
            assert not peer.errors, peer.errors
            # A response through the real client also proves it remains usable.
            time.sleep(.5)
        peer.send(11, proto.TextMessage(session=[user.session], message="soak-complete compatibility text"))
        wait(lambda: received_text("soak-complete compatibility text"))
        ImageGrab.grab().save(root / "soak-complete.png")
        assert not peer.errors, peer.errors
        assert client.poll() is None and server.poll() is None, "Process exited before final verification"
        result.update(pass_=True, connected_seconds=round(time.monotonic() - started, 2),
                      observer_frames=peer.received, observer_errors=list(peer.errors))
        result["pass"] = result.pop("pass_")
    except Exception as error:
        result["error"] = repr(error)
        ImageGrab.grab().save(root / "failure.png")
        with (root / "accessibility.txt").open("w") as stream:
            for node in walk(pyatspi.Registry.getDesktop(0)):
                try:
                    stream.write(f"{node.getRoleName()}: {node.name}\n")
                except Exception:
                    pass
    finally:
        if client and client.poll() is None:
            # Liveness was checked through the final text exchange. SIGTERM is
            # fixture cleanup after that measurement, not a spontaneous crash.
            result["intentional_shutdown"] = True
            # A hung client (see docs/dev/upstream-sync/2026-10-21/shutdown-hang.bt) is killed
            # and recorded, so the remaining cleanup and result.json still happen.
            result["client_exit"] = stop_process(client, result, "client_shutdown_timeout")
        if peer:
            peer.close()
        # Let the server finish processing the fixture disconnects before signaling shutdown.
        time.sleep(.3)
        stop_process(server, result, "server_shutdown_timeout")
        server_log.close()
        client_log.close()
        observer_log.close()
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)
    return not result["pass"]


if __name__ == "__main__":
    raise SystemExit(main())
