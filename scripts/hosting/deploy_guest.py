#!/usr/bin/env python3
"""Install one pinned private preview on a fresh Debian 12 amd64 systemd guest.

Run as root with a reviewed manifest and local .deb. This rehearsal installer
owns its configuration; it refuses existing servers, changed files and upgrades.
It does not purchase a VPS, publish a listing, or qualify a stable release.
"""
import argparse
from contextlib import closing
import fcntl
import grp
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

STATE = Path('/var/lib/mumble-hosting')
INI = Path('/etc/mumble/mumble-server.ini')
DATA = Path('/var/lib/mumble-server')
DROPIN = Path('/etc/systemd/system/mumble-server.service.d/hosting.conf')
FIREWALL = Path('/etc/mumble-hosting/firewall.nft')
FW_UNIT = Path('/etc/systemd/system/mumble-hosting-firewall.service')


def run(*argv, input=None):
    # Never print command output: server/config errors can contain credentials.
    result = subprocess.run(argv, input=input, text=True, capture_output=True,
                            timeout=300, env={**os.environ, 'DEBIAN_FRONTEND': 'noninteractive'})
    if result.returncode:
        raise RuntimeError(f'{Path(argv[0]).name} failed (exit {result.returncode}); output withheld')
    return result.stdout.strip()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def atomic(path, text, mode=0o600):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.hosting-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def save(state):
    atomic(STATE / 'deployment.json', json.dumps(state, indent=2) + '\n')


def initialize_database(ini, uid, gid, port):
    # A fresh DB has no server or SuperUser until its first normal boot. Keep
    # that bootstrap listener on guest loopback, then set the owned password.
    bootstrap = DATA / 'bootstrap.ini'
    atomic(bootstrap, ini.replace('host=0.0.0.0\n', 'host=127.0.0.1\n'), 0o600)
    os.chown(bootstrap, uid, gid)
    diagnostic = STATE / 'bootstrap.log'
    with diagnostic.open('w') as log:
        diagnostic.chmod(0o600)
        process = subprocess.Popen(['/usr/bin/mumble-server', '--ini', str(bootstrap), '--foreground'],
                                   user=uid, group=gid, extra_groups=[],
                                   stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('Loopback database initialization failed; output withheld')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=1):
                    return
            except OSError:
                time.sleep(.2)
        raise RuntimeError('Loopback database initialization timed out')
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('Database bootstrap did not stop cleanly')
        bootstrap.unlink(missing_ok=True)


def validate_manifest(manifest, package):
    required = {'schema', 'source_commit', 'package_sha256', 'package_version', 'os', 'architecture'}
    if set(manifest) != required or manifest['schema'] != 1:
        raise ValueError('Invalid deployment manifest fields/schema')
    if manifest['os'] != 'debian-12' or manifest['architecture'] != 'amd64':
        raise ValueError('Only Debian 12 amd64 is supported by this rehearsal')
    if not re.fullmatch(r'[a-f0-9]{40}', manifest['source_commit']):
        raise ValueError('Manifest requires a complete source commit')
    if not re.fullmatch(r'[a-f0-9]{64}', manifest['package_sha256']):
        raise ValueError('Manifest requires a SHA-256 package digest')
    if not re.fullmatch(r'[0-9A-Za-z.+:~\-]+', manifest['package_version']):
        raise ValueError('Invalid package version')
    if digest(package) != manifest['package_sha256']:
        raise ValueError('Package digest mismatch; nothing installed')
    actual = run('dpkg-deb', '-f', str(package), 'Package', 'Version', 'Architecture').splitlines()
    expected = ['Package: mumble-server', 'Version: ' + manifest['package_version'], 'Architecture: amd64']
    if actual != expected:
        raise ValueError('Package metadata does not match manifest')


def deploy(args):
    manifest = json.loads(args.manifest.read_text())
    package = args.package.resolve(strict=True)
    validate_manifest(manifest, package)
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'debian' or release.get('VERSION_ID', '').strip('"') != '12':
        raise ValueError('Fresh Debian 12 guest required')
    if run('dpkg', '--print-architecture') != 'amd64' or not Path('/run/systemd/system').is_dir():
        raise ValueError('amd64 guest with running systemd required')
    config = {'port': args.port, 'ssh_port': args.ssh_port, 'endpoint': args.endpoint}
    marker = STATE / 'deployment.json'
    if marker.exists():
        state = json.loads(marker.read_text())
        if state.get('manifest') != manifest or state.get('config') != config:
            raise ValueError('Managed deployment differs; explicit migration/rollback is required')
        if state.get('phase') == 'ready':
            for name, expected in state['managed_files'].items():
                if not Path(name).is_file() or digest(Path(name)) != expected:
                    raise ValueError('Managed configuration changed; refusing to overwrite')
            if run('dpkg-query', '-W', '-f=${Version}', 'mumble-server') != manifest['package_version']:
                raise ValueError('Installed version changed; refusing automatic repair')
            run('systemctl', 'start', 'mumble-hosting-firewall.service')
            run('systemctl', 'start', 'mumble-server.service')
            return {'status': 'unchanged', 'source_commit': manifest['source_commit'],
                    'credentials_file': str(STATE / 'credentials.json')}
    else:
        existing = subprocess.run(['dpkg-query', '-W', '-f=${Status}', 'mumble-server'],
                                  text=True, capture_output=True)
        if (existing.returncode == 0 and 'installed' in existing.stdout) or INI.exists() or DATA.exists():
            raise ValueError('Existing server/state found; fresh guest required')
        if FIREWALL.exists() or FW_UNIT.exists() or DROPIN.exists() or STATE.exists():
            raise ValueError('Unowned hosting configuration found')
        STATE.mkdir(mode=0o700)
        state = {'manifest': manifest, 'config': config, 'phase': 'preparing'}
        save(state)
    # Record ownership before installing so interrupted preparation can resume.
    if subprocess.run(['systemctl', 'is-active', '--quiet', 'mumble-server.service']).returncode == 0:
        run('systemctl', 'stop', 'mumble-server.service')
    pinned = STATE / 'mumble-server.deb'
    if not pinned.exists() or digest(pinned) != manifest['package_sha256']:
        shutil.copyfile(package, pinned)
        pinned.chmod(0o600)
    # apt's sandbox user must read the package; use a public temporary copy.
    with tempfile.TemporaryDirectory(prefix='mumble-package-', dir='/var/tmp') as directory:
        os.chmod(directory, 0o755)
        staged = Path(directory) / 'mumble-server.deb'
        shutil.copyfile(pinned, staged)
        staged.chmod(0o644)
        run('apt-get', 'update')
        run('apt-get', 'install', '-y', str(staged), 'openssl', 'nftables', 'python3')
    gid = grp.getgrnam('_mumble-server').gr_gid
    DATA.mkdir(mode=0o750, exist_ok=True)
    uid = int(run('id', '-u', '_mumble-server'))
    os.chown(DATA, uid, gid)
    credentials = STATE / 'credentials.json'
    if not credentials.exists():
        atomic(credentials, json.dumps({'join_password': secrets.token_hex(24),
                                        'superuser_password': secrets.token_hex(24)}, indent=2) + '\n')
    passwords = json.loads(credentials.read_text())
    cert, key = DATA / 'server.crt', DATA / 'server.key'
    if cert.exists() != key.exists():
        raise ValueError('Incomplete TLS identity; manual recovery required')
    if not cert.exists():
        run('openssl', 'req', '-x509', '-newkey', 'rsa:3072', '-nodes', '-days', '365',
            '-subj', '/CN=' + args.endpoint, '-addext', 'subjectAltName=DNS:' + args.endpoint,
            '-keyout', str(key), '-out', str(cert))
    for path in (key, cert):
        path.chmod(0o640)
        os.chown(path, 0, gid)
    # Conservative private voice baseline. Video limits are not a qualified plan.
    ini = (f'database={DATA}/server.sqlite\ndbDriver=sqlite\nsqlite_wal=2\nlogfile=\n'
           f'host=0.0.0.0\nport={args.port}\nusers=10\nbandwidth=72000\n'
           'videobandwidth=2500000\nvideobandwidthaggregate=20000000\n'
           f'sslCert={cert}\nsslKey={key}\nserverpassword={passwords["join_password"]}\n'
           'allowping=false\nautobanSuccessfulConnections=false\nwelcometext=Private deployment rehearsal\n')
    atomic(INI, ini, 0o640)
    os.chown(INI, 0, gid)
    atomic(DROPIN, '[Unit]\nRequires=mumble-hosting-firewall.service\nAfter=mumble-hosting-firewall.service\n'
           '[Service]\nStateDirectory=mumble-server\nStateDirectoryMode=0750\nMemoryMax=512M\n'
           'CPUQuota=100%\nTasksMax=128\nLimitNOFILE=4096\nRestartSec=3\n', 0o644)
    atomic(FIREWALL, 'table inet mumble_hosting {\n chain input {\n'
           ' type filter hook input priority 0; policy drop;\n'
           ' iifname "lo" accept\n ct state established,related accept\n'
           ' ip protocol icmp accept\n meta l4proto ipv6-icmp accept\n'
           ' udp sport 67 udp dport 68 accept\n'
           f' tcp dport {{ {args.ssh_port}, {args.port} }} accept\n udp dport {args.port} accept\n'
           ' }\n}\n', 0o600)
    atomic(FW_UNIT, '[Unit]\nDescription=Private Mumble guest ingress rules\nBefore=mumble-server.service\n'
           '[Service]\nType=oneshot\nRemainAfterExit=yes\n'
           'ExecStartPre=-/usr/sbin/nft delete table inet mumble_hosting\n'
           f'ExecStart=/usr/sbin/nft -f {FIREWALL}\n'
           '[Install]\nWantedBy=multi-user.target\n', 0o644)
    # Password travels through stdin, never command arguments or normal output.
    initialize_database(ini, uid, gid, args.port)
    with closing(sqlite3.connect(f'file:{DATA}/server.sqlite?mode=ro', uri=True)) as database:
        server_ids = database.execute('SELECT server_id FROM users WHERE user_id = 0').fetchall()
    if len(server_ids) != 1:
        raise ValueError('Expected exactly one initialized SuperUser account')
    server_id = str(server_ids[0][0])
    run('runuser', '-u', '_mumble-server', '--', '/usr/bin/mumble-server', '--ini', str(INI),
        '--read-su-pw', server_id, input=passwords['superuser_password'] + '\n')
    with closing(sqlite3.connect(f'file:{DATA}/server.sqlite?mode=ro', uri=True)) as database:
        stored_hash, salt, iterations = database.execute(
            'SELECT password_hash, salt, kdf_iterations FROM users WHERE server_id = ? AND user_id = 0',
            (int(server_id),)).fetchone()
    if not stored_hash or not salt or not iterations or not hmac.compare_digest(
            hashlib.pbkdf2_hmac('sha384', passwords['superuser_password'].encode(),
                               bytes.fromhex(salt), iterations, 48).hex(), stored_hash):
        raise RuntimeError('Stored administrator password verification failed')
    run('nft', '--check', '-f', str(FIREWALL))
    run('systemctl', 'daemon-reload')
    run('systemctl', 'enable', '--now', 'mumble-hosting-firewall.service', 'mumble-server.service')
    time.sleep(1)
    run('systemctl', 'is-active', 'mumble-server.service')
    state['phase'] = 'ready'
    state['managed_files'] = {str(path): digest(path) for path in (INI, DROPIN, FIREWALL, FW_UNIT, cert, key, credentials)}
    save(state)
    return {'status': 'installed', 'source_commit': manifest['source_commit'],
            'credentials_file': str(credentials), 'port': args.port}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--port', type=int, default=64738)
    parser.add_argument('--ssh-port', type=int, default=22)
    parser.add_argument('--endpoint', required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not 1 <= args.ssh_port <= 65535 or args.port == args.ssh_port:
        parser.error('Distinct valid service/SSH ports required (service port >= 1024)')
    if not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?', args.endpoint):
        parser.error('A DNS endpoint name is required')
    if os.geteuid() != 0:
        parser.error('Run as root inside the designated disposable guest')
    os.umask(0o077)
    try:
        with Path('/run/lock/mumble-hosting.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            print(json.dumps(deploy(args)))
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        # Do not include arbitrary external output or configuration in errors.
        print(f'Deployment stopped: {error.__class__.__name__}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
