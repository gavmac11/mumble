#!/usr/bin/env python3
"""Encrypted recovery of the pinned Debian 1.3.4/schema-7 rehearsal snapshot.

This explicitly owned historical profile restores on a fresh Debian 12 guest,
using the verified package's extracted binary and a separate dedicated unit.
It does not replace an existing managed deployment or downgrade a live database.
"""
import argparse
from contextlib import closing
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import socket
import ssl
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time

import deploy_guest as deployment
from guest_state import encrypt_archive

MAX_ARCHIVE = 64 * 1024 * 1024
ROOT = Path('/var/lib/mumble-historical-rehearsal')
STATE = ROOT / 'state'
DATA = ROOT / 'data'
UNIT = Path('/etc/systemd/system/mumble-historical-rehearsal.service')
FW_UNIT = Path('/etc/systemd/system/mumble-historical-firewall.service')
USER = '_mumble-history'
PACKAGE_NAME = 'mumble-server_1.3.4-4_amd64.deb'
PACKAGE_SHA = 'c7381408f4c2078844156ce7c38041648f7012ffc8eb0525e7855f7040966d5e'
PROFILE = {'format': 1, 'profile': 'debian-12-historical-rehearsal',
           'package_version': '1.3.4-4', 'package_sha256': PACKAGE_SHA, 'database_schema': 7}
NAMES = ('server.sqlite', 'server.ini', 'server.crt', 'server.key', PACKAGE_NAME)
INI_KEYS = {'database', 'dbDriver', 'sqlite_wal', 'logfile', 'host', 'port', 'users',
            'bandwidth', 'sslCert', 'sslKey', 'serverpassword', 'allowping',
            'autobanSuccessfulConnections', 'welcometext'}


def legacy_schema(path):
    with closing(sqlite3.connect(f'file:{path}?mode=ro', uri=True)) as db:
        if db.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise ValueError('Historical SQLite integrity check failed')
        if [row[1] for row in db.execute('PRAGMA table_info(meta)')] != ['keystring', 'value']:
            raise ValueError('Unsupported historical metadata layout')
        row = db.execute("SELECT value FROM meta WHERE keystring = 'version'").fetchone()
        if not row or row[0] != '7':
            raise ValueError('Pinned older binary requires a schema-7 snapshot')
    return 7


def parse_ini(path):
    result = {}
    for line in path.read_text().splitlines():
        if '=' not in line:
            raise ValueError('Unsupported historical INI syntax')
        key, value = line.split('=', 1)
        if key not in INI_KEYS or key in result:
            raise ValueError('Unsupported or duplicate historical INI setting')
        result[key] = value
    if set(result) != INI_KEYS or result['dbDriver'] != 'QSQLITE':
        raise ValueError('Historical configuration inventory/driver mismatch')
    if result['host'] != '127.0.0.1' or result['port'] != '64739' or result['logfile']:
        raise ValueError('Only the explicit private historical rehearsal source is supported')
    if not re.fullmatch(r'[a-f0-9]{48}', result['serverpassword']):
        raise ValueError('Historical join credential is missing or invalid')
    for key, name in (('database', 'server.sqlite'), ('sslCert', 'server.crt'), ('sslKey', 'server.key')):
        value = Path(result[key])
        if not value.is_absolute() or value.name != name:
            raise ValueError('Unexpected historical source path')
    if len({Path(result[key]).parent for key in ('database', 'sslCert', 'sslKey')}) != 1:
        raise ValueError('Historical source paths do not share their owned directory')
    return result


def render_ini(settings, port):
    if not 1024 <= port <= 65535:
        raise ValueError('Historical destination port is invalid')
    settings = dict(settings)
    settings.update(database=str(DATA / 'server.sqlite'), host='0.0.0.0', port=str(port),
                    sslCert=str(ROOT / 'server.crt'), sslKey=str(ROOT / 'server.key'))
    return ''.join(f'{key}={value}\n' for key, value in settings.items())


def export(args):
    reviewed = json.loads((args.snapshot / 'manifest.json').read_text())
    if reviewed.get('schema_version') != 7 or reviewed.get('legacy_version') != '1.3.4-4' \
            or set(reviewed.get('files', {})) != set(NAMES):
        raise ValueError('Reviewed historical snapshot inventory is invalid')
    if not re.fullmatch(r'age1[0-9a-z]{58}', args.recipient):
        raise ValueError('An age X25519 public recipient is required')
    if args.output.exists():
        raise ValueError('Encrypted historical output already exists')
    for name in NAMES:
        path = args.snapshot / name
        if path.is_symlink() or not path.is_file() or deployment.digest(path) != reviewed['files'][name]:
            raise ValueError('Historical snapshot payload differs from its reviewed hash')
    if deployment.digest(args.snapshot / PACKAGE_NAME) != PACKAGE_SHA:
        raise ValueError('Historical package pin mismatch')
    if sum((args.snapshot / name).stat().st_size for name in NAMES) > MAX_ARCHIVE // 2:
        raise ValueError('Historical snapshot exceeds the bounded archive size')
    legacy_schema(args.snapshot / 'server.sqlite')
    parse_ini(args.snapshot / 'server.ini')
    with tempfile.TemporaryDirectory(prefix='mumble-history-export-', dir='/run') as directory:
        stage = Path(directory)
        with closing(sqlite3.connect(f'file:{args.snapshot}/server.sqlite?mode=ro', uri=True)) as db, \
                closing(sqlite3.connect(stage / 'server.sqlite')) as target:
            db.backup(target)
        for name in NAMES[1:]:
            shutil.copyfile(args.snapshot / name, stage / name)
        legacy_schema(stage / 'server.sqlite')
        metadata = {**PROFILE, 'files': {name: {'sha256': deployment.digest(stage / name),
                                              'bytes': (stage / name).stat().st_size} for name in NAMES}}
        deployment.atomic(stage / 'backup.json', json.dumps(metadata, indent=2) + '\n')
        archive = stage / 'state.tar'
        with tarfile.open(archive, 'w', format=tarfile.PAX_FORMAT) as stream:
            for name in (*NAMES, 'backup.json'):
                stream.add(stage / name, arcname=name, recursive=False)
        if archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError('Historical archive exceeds the bound')
        encrypt_archive(archive, args.recipient, args.output)
    return {'status': 'exported', 'profile': PROFILE, 'sha256': deployment.digest(args.output),
            'bytes': args.output.stat().st_size, 'encryption_requires_public_recipient_only': True}


def unpack(archive, destination):
    with tarfile.open(archive, 'r:') as stream:
        members = stream.getmembers()
        names = [member.name for member in members]
        if len(names) != len(set(names)) or set(names) != {*NAMES, 'backup.json'}:
            raise ValueError('Historical archive inventory is invalid')
        if any(not member.isfile() or member.size < 0 for member in members) \
                or sum(member.size for member in members) > MAX_ARCHIVE:
            raise ValueError('Historical archive contains an invalid payload')
        for member in members:
            with stream.extractfile(member) as source, (destination / member.name).open('xb') as target:
                shutil.copyfileobj(source, target, 128 * 1024)
    metadata = json.loads((destination / 'backup.json').read_text())
    if any(metadata.get(key) != value for key, value in PROFILE.items()) \
            or set(metadata.get('files', {})) != set(NAMES):
        raise ValueError('Historical profile does not match the reviewed version')
    for name, record in metadata['files'].items():
        if deployment.digest(destination / name) != record.get('sha256') \
                or (destination / name).stat().st_size != record.get('bytes'):
            raise ValueError('Historical payload hash/size mismatch')
    if deployment.digest(destination / PACKAGE_NAME) != PACKAGE_SHA:
        raise ValueError('Historical package pin mismatch')
    legacy_schema(destination / 'server.sqlite')
    parse_ini(destination / 'server.ini')
    return metadata


def fresh_or_owned(sha256, port):
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'debian' or release.get('VERSION_ID', '').strip('"') != '12' \
            or deployment.run('dpkg', '--print-architecture') != 'amd64' or not Path('/run/systemd/system').is_dir():
        raise ValueError('Debian 12 amd64 systemd guest required')
    if any(path.is_symlink() for path in (ROOT, STATE, UNIT, FW_UNIT)):
        raise ValueError('Historical restore paths contain pre-existing links')
    marker = STATE / 'restore.json'
    if marker.exists():
        state = json.loads(marker.read_text())
        if state.get('phase') != 'restoring' or state.get('sha256') != sha256 \
                or state.get('profile') != PROFILE or state.get('port') != port:
            raise ValueError('Existing historical state does not belong to this restore')
        return
    installed = subprocess.run(['dpkg-query', '-W', '-f=${Status}', 'mumble-server'],
                               capture_output=True, text=True)
    if installed.returncode == 0 and 'installed' in installed.stdout:
        raise ValueError('Existing Mumble package; a fresh guest is required')
    if any(path.exists() or path.is_symlink() for path in (ROOT, UNIT, FW_UNIT, deployment.STATE, deployment.DATA,
                                     deployment.INI, Path('/etc/mumble-server.ini'))):
        raise ValueError('Existing Mumble state; a fresh guest is required')
    ROOT.mkdir(mode=0o750)
    ROOT.chmod(0o750)
    STATE.mkdir(mode=0o700)
    deployment.atomic(marker, json.dumps({'phase': 'restoring', 'sha256': sha256,
                                         'profile': PROFILE, 'port': port}) + '\n')


def local_tls_ready(port, certificate, timeout=10):
    context = ssl.create_default_context(cafile=str(certificate))
    # Local readiness pins the exact archived leaf, independent of its public DNS name.
    # The separate external probe still validates the public hostname and actual accounts.
    context.check_hostname = False
    expected = ssl.PEM_cert_to_DER_cert(certificate.read_text())
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1) as connection, \
                    context.wrap_socket(connection) as secure:
                if secure.getpeercert(binary_form=True) != expected:
                    raise ValueError('Historical listener presents a different TLS identity')
            return
        except (OSError, ssl.SSLError):
            if time.monotonic() >= deadline:
                raise RuntimeError('Historical TLS listener did not become ready')
            time.sleep(.1)


def restore(args):
    started = time.monotonic()
    if not re.fullmatch(r'[a-f0-9]{64}', args.sha256) or deployment.digest(args.archive) != args.sha256:
        raise ValueError('Historical ciphertext differs from the trusted operator pin')
    if not 1024 <= args.port <= 65535 or args.archive.stat().st_size > MAX_ARCHIVE + 1024 * 1024:
        raise ValueError('Historical restore input exceeds its bounds')
    with tempfile.TemporaryDirectory(prefix='mumble-history-restore-', dir='/run') as directory:
        temporary = Path(directory)
        archive = temporary / 'state.tar'
        deployment.run('age', '--decrypt', '--identity', str(args.identity), '--output', str(archive), str(args.archive))
        if archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError('Decrypted historical archive exceeds its bound')
        stage = temporary / 'payload'
        stage.mkdir(mode=0o700)
        metadata = unpack(archive, stage)
        # No server state is touched until every cipher/package/database/config check passes.
        fresh_or_owned(args.sha256, args.port)
        if subprocess.run(['systemctl', 'is-active', '--quiet', UNIT.name],
                          capture_output=True).returncode == 0:
            deployment.run('systemctl', 'stop', UNIT.name)
        deployment.run('apt-get', 'update')
        deployment.run('apt-get', 'install', '-y', '--no-install-recommends', 'libqt5core5a',
                       'libqt5network5', 'libqt5sql5', 'libqt5sql5-sqlite', 'libqt5xml5', 'libqt5dbus5',
                       'libcap2', 'libzeroc-ice3.7', 'libssl3', 'libprotobuf32',
                       'libavahi-compat-libdnssd1', 'nftables')
        try:
            user = pwd.getpwnam(USER)
        except KeyError:
            deployment.run('useradd', '--system', '--no-create-home', '--shell', '/usr/sbin/nologin', USER)
            user = pwd.getpwnam(USER)
        os.chown(ROOT, 0, user.pw_gid)
        DATA.mkdir(mode=0o750, exist_ok=True)
        DATA.chmod(0o750)
        os.chown(DATA, user.pw_uid, user.pw_gid)
        for suffix in ('-wal', '-shm'):
            (DATA / ('server.sqlite' + suffix)).unlink(missing_ok=True)
        for name, target in (('server.sqlite', DATA / 'server.sqlite'), ('server.crt', ROOT / 'server.crt'),
                             ('server.key', ROOT / 'server.key')):
            shutil.copyfile(stage / name, target)
            target.chmod(0o600 if name == 'server.sqlite' else 0o640)
            os.chown(target, user.pw_uid if name == 'server.sqlite' else 0, user.pw_gid)
            if deployment.digest(target) != metadata['files'][name]['sha256']:
                raise ValueError('Installed historical database/TLS payload differs')
        for name in (PACKAGE_NAME, 'server.ini', 'backup.json'):
            shutil.copyfile(stage / name, STATE / name)
            (STATE / name).chmod(0o600)
        runtime = ROOT / 'runtime'
        runtime.mkdir(mode=0o755, exist_ok=True)
        runtime.chmod(0o755)
        deployment.run('dpkg-deb', '-x', str(STATE / PACKAGE_NAME), str(runtime))
        configuration = render_ini(parse_ini(stage / 'server.ini'), args.port)
        deployment.atomic(ROOT / 'server.ini', configuration, 0o640)
        os.chown(ROOT / 'server.ini', 0, user.pw_gid)
        deployment.atomic(UNIT, f'''[Unit]
Description=Owned historical Mumble recovery rehearsal
After=network.target
[Service]
User={USER}
Group={USER}
ExecStart={runtime}/usr/sbin/murmurd -ini {ROOT}/server.ini -fg
WorkingDirectory={DATA}
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths={DATA}
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
MemoryMax=512M
CPUQuota=100%
TasksMax=128
[Install]
WantedBy=multi-user.target
''', 0o644)
        firewall = ROOT / 'firewall.nft'
        deployment.atomic(firewall, f'''table inet mumble_history {{
 chain input {{
  type filter hook input priority 0; policy drop;
  iifname "lo" accept
  ct state established,related accept
  tcp dport {{ 22, {args.port} }} accept
  udp dport {args.port} accept
  ip protocol icmp accept
  ip6 nexthdr ipv6-icmp accept
 }}
}}
''', 0o600)
        deployment.atomic(FW_UNIT, f'''[Unit]
Description=Owned historical rehearsal firewall
Before=mumble-historical-rehearsal.service
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/nft -f {firewall}
ExecStop=/usr/sbin/nft delete table inet mumble_history
[Install]
WantedBy=multi-user.target
''', 0o644)
        deployment.run('nft', '--check', '-f', str(firewall))
        deployment.run('systemctl', 'daemon-reload')
        deployment.run('systemctl', 'enable', '--now', FW_UNIT.name, UNIT.name)
        local_tls_ready(args.port, ROOT / 'server.crt')
        deployment.run('systemctl', 'is-active', UNIT.name)
        deployment.atomic(STATE / 'restore.json', json.dumps({'phase': 'ready', 'sha256': args.sha256,
                                                            'profile': PROFILE, 'port': args.port}) + '\n')
    return {'status': 'restored', 'profile': PROFILE, 'database_tls_hashes_match': True,
            'local_tls_certificate_matches': True, 'original_ini_retained': True, 'configuration_overrides': ['database', 'sslCert', 'sslKey', 'host', 'port'],
            'seconds': round(time.monotonic() - started, 3), 'external_readiness_required': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest='command', required=True)
    backup = subcommands.add_parser('export')
    backup.add_argument('--snapshot', type=Path, required=True)
    backup.add_argument('--recipient', required=True)
    backup.add_argument('--output', type=Path, required=True)
    recover = subcommands.add_parser('restore')
    recover.add_argument('--archive', type=Path, required=True)
    recover.add_argument('--sha256', required=True)
    recover.add_argument('--identity', type=Path, required=True)
    recover.add_argument('--port', type=int, default=64738)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run as root inside the explicitly designated historical/disposable guest')
    os.umask(0o077)
    try:
        with Path('/run/lock/mumble-historical.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            print(json.dumps(export(args) if args.command == 'export' else restore(args)))
    except (ValueError, OSError, RuntimeError, sqlite3.DatabaseError, tarfile.TarError,
            subprocess.TimeoutExpired) as error:
        print(f'Historical operation stopped: {type(error).__name__}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
