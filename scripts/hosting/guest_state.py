#!/usr/bin/env python3
"""Encrypted portable state for the managed private Debian guest rehearsal.

Backups briefly stop the managed server and use SQLite's backup API. Restore
requires a trusted ciphertext digest, reviewed package manifest and fresh guest.
Only the standard managed SQLite layout is supported; no DB downgrade is done.
"""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import fcntl
import grp
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time

import deploy_guest as deployment

MAX_ARCHIVE = 256 * 1024 * 1024
SPOOL_ROOT = Path('/run')
PAYLOADS = {
    'server.sqlite': deployment.DATA / 'server.sqlite',
    'server.crt': deployment.DATA / 'server.crt',
    'server.key': deployment.DATA / 'server.key',
    'server.ini': deployment.INI,
    'hosting.conf': deployment.DROPIN,
    'firewall.nft': deployment.FIREWALL,
    'firewall.service': deployment.FW_UNIT,
    'credentials.json': deployment.STATE / 'credentials.json',
    'deployment.json': deployment.STATE / 'deployment.json',
    'mumble-server.deb': deployment.STATE / 'mumble-server.deb',
}


def schema_version(database):
    row = database.execute("SELECT meta_value FROM meta WHERE meta_key = 'schema_version'").fetchone()
    if not row or not str(row[0]).isdigit():
        raise ValueError('Database schema version is missing or invalid')
    return int(row[0])


def snapshot_database(source, destination):
    with closing(sqlite3.connect(f'file:{source}?mode=ro', uri=True)) as original, \
            closing(sqlite3.connect(destination)) as snapshot:
        original.backup(snapshot)
        if snapshot.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise ValueError('SQLite backup failed its integrity check')
        return schema_version(snapshot)


def service_active():
    return subprocess.run(['systemctl', 'is-active', '--quiet', 'mumble-server.service'],
                          capture_output=True).returncode == 0


def managed_state():
    state = json.loads(PAYLOADS['deployment.json'].read_text())
    if state.get('phase') != 'ready':
        raise ValueError('A ready managed deployment is required')
    expected = {str(path) for name, path in PAYLOADS.items()
                if name not in ('server.sqlite', 'deployment.json', 'mumble-server.deb')}
    if set(state.get('managed_files', {})) != expected:
        raise ValueError('Managed configuration inventory does not match this tool')
    for path, expected_hash in state['managed_files'].items():
        if not Path(path).is_file() or deployment.digest(Path(path)) != expected_hash:
            raise ValueError('Managed configuration changed; export/migration must be reviewed')
    deployment.validate_manifest(state['manifest'], PAYLOADS['mumble-server.deb'])
    if deployment.run('dpkg-query', '-W', '-f=${Version}', 'mumble-server') != state['manifest']['package_version']:
        raise ValueError('Installed server version differs from the owned package pin')
    return state


def encrypt_archive(source, recipient, output):
    if output.exists():
        raise ValueError('Backup output already exists; refusing to overwrite')
    # Ciphertext is staged on the output filesystem, then published without an
    # overwrite race. The plaintext spool stays in the private /run directory.
    fd, temporary = tempfile.mkstemp(prefix='.mumble-encrypted-', dir=output.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            result = subprocess.run(['age', '--recipient', recipient, str(source)],
                                    stdout=stream, stderr=subprocess.PIPE, timeout=300)
            if result.returncode:
                raise RuntimeError('age encryption failed; output withheld')
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, output)
    finally:
        Path(temporary).unlink(missing_ok=True)


def backup(args):
    if args.output.exists():
        raise ValueError('Backup output already exists; refusing to overwrite')
    if not re.fullmatch(r'age1[0-9a-z]{58}', args.recipient):
        raise ValueError('An age X25519 recipient is required')
    state = managed_state()
    if sum(path.stat().st_size for path in PAYLOADS.values()) > MAX_ARCHIVE // 2:
        raise ValueError('State exceeds the bounded rehearsal archive size')
    started = time.monotonic()
    was_active = service_active()
    downtime = 0.0
    with tempfile.TemporaryDirectory(prefix='mumble-state-', dir=SPOOL_ROOT) as directory:
        stage = Path(directory)
        stopped_at = time.monotonic()
        try:
            if was_active:
                deployment.run('systemctl', 'stop', 'mumble-server.service')
            version = snapshot_database(PAYLOADS['server.sqlite'], stage / 'server.sqlite')
            for name, source in PAYLOADS.items():
                if name != 'server.sqlite':
                    shutil.copyfile(source, stage / name)
            metadata = {'schema': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                        'package_manifest': state['manifest'], 'database_schema': version,
                        'files': {name: {'sha256': deployment.digest(stage / name),
                                         'bytes': (stage / name).stat().st_size} for name in PAYLOADS}}
            deployment.atomic(stage / 'backup.json', json.dumps(metadata, indent=2) + '\n')
        finally:
            if was_active:
                deployment.run('systemctl', 'start', 'mumble-server.service')
                deployment.run('systemctl', 'is-active', 'mumble-server.service')
            downtime = time.monotonic() - stopped_at if was_active else 0.0
        archive = stage / 'state.tar'
        with tarfile.open(archive, 'w', format=tarfile.PAX_FORMAT) as stream:
            for name in [*PAYLOADS, 'backup.json']:
                stream.add(stage / name, arcname=name, recursive=False)
        if archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError('Archive exceeds the bounded rehearsal size')
        encrypt_archive(archive, args.recipient, args.output)
    return {'status': 'backed_up', 'backup_sha256': deployment.digest(args.output),
            'source_commit': state['manifest']['source_commit'], 'database_schema': version,
            'service_pause_seconds': round(downtime, 3),
            'total_seconds': round(time.monotonic() - started, 3),
            'bytes': args.output.stat().st_size}


def unpack_verified_archive(archive, stage, expected_manifest):
    expected_names = {*PAYLOADS, 'backup.json'}
    with tarfile.open(archive, 'r:') as stream:
        members = stream.getmembers()
        names = [member.name for member in members]
        if len(names) != len(set(names)) or set(names) != expected_names:
            raise ValueError('Backup archive inventory is invalid')
        if any(not member.isfile() or member.size < 0 for member in members):
            raise ValueError('Backup archive contains a non-regular payload')
        if sum(member.size for member in members) > MAX_ARCHIVE:
            raise ValueError('Backup payload exceeds the rehearsal size limit')
        # Do not use extract/extractall: links, paths and archive permissions are
        # never applied. Each fixed, validated name receives a newly owned file.
        for member in members:
            with stream.extractfile(member) as source, (stage / member.name).open('xb') as output:
                shutil.copyfileobj(source, output, 128 * 1024)
    metadata = json.loads((stage / 'backup.json').read_text())
    if (metadata.get('schema') != 1 or metadata.get('package_manifest') != expected_manifest
            or set(metadata.get('files', {})) != set(PAYLOADS)):
        raise ValueError('Backup metadata does not match the reviewed candidate')
    for name, expected in metadata['files'].items():
        if (deployment.digest(stage / name) != expected.get('sha256')
                or (stage / name).stat().st_size != expected.get('bytes')):
            raise ValueError('Backup payload digest/size mismatch')
    deployment.validate_manifest(expected_manifest, stage / 'mumble-server.deb')
    state = json.loads((stage / 'deployment.json').read_text())
    if state.get('phase') != 'ready' or state.get('manifest') != expected_manifest:
        raise ValueError('Backup deployment state does not match the package')
    expected_managed = {str(path): metadata['files'][name]['sha256'] for name, path in PAYLOADS.items()
                        if name not in ('server.sqlite', 'deployment.json', 'mumble-server.deb')}
    if state.get('managed_files') != expected_managed:
        raise ValueError('Backup managed configuration inventory is inconsistent')
    with closing(sqlite3.connect(f'file:{stage}/server.sqlite?mode=ro', uri=True)) as database:
        if database.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise ValueError('Restored SQLite payload failed its integrity check')
        if schema_version(database) != metadata.get('database_schema'):
            raise ValueError('Restored database schema differs from the backup')
    return metadata, state


def fresh_or_resumable(expected_hash, manifest):
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if (release.get('ID', '').strip('"') != 'debian' or release.get('VERSION_ID', '').strip('"') != '12'
            or deployment.run('dpkg', '--print-architecture') != 'amd64'
            or not Path('/run/systemd/system').is_dir()):
        raise ValueError('Debian 12 amd64 systemd guest required')
    marker = deployment.STATE / 'deployment.json'
    if marker.exists():
        state = json.loads(marker.read_text())
        if (state.get('phase') != 'restoring' or state.get('restore_sha256') != expected_hash
                or state.get('manifest') != manifest):
            raise ValueError('Existing deployment found; restore requires a fresh guest')
    else:
        installed = subprocess.run(['dpkg-query', '-W', '-f=${Status}', 'mumble-server'],
                                   capture_output=True, text=True)
        if (installed.returncode == 0 and 'installed' in installed.stdout) or any(
                path.exists() for path in (deployment.STATE, deployment.DATA, deployment.INI,
                                          deployment.DROPIN, deployment.FIREWALL, deployment.FW_UNIT)):
            raise ValueError('Existing server/state found; restore requires a fresh guest')
        deployment.STATE.mkdir(mode=0o700)
        deployment.save({'phase': 'restoring', 'restore_sha256': expected_hash, 'manifest': manifest})


def restore(args):
    started = time.monotonic()
    if not re.fullmatch(r'[a-f0-9]{64}', args.sha256) or deployment.digest(args.archive) != args.sha256:
        raise ValueError('Ciphertext digest does not match the trusted backup pin')
    if args.archive.stat().st_size > MAX_ARCHIVE + 1024 * 1024:
        raise ValueError('Encrypted backup exceeds the rehearsal size limit')
    manifest = json.loads(args.manifest.read_text())
    with tempfile.TemporaryDirectory(prefix='mumble-restore-', dir=SPOOL_ROOT) as directory:
        temporary = Path(directory)
        archive = temporary / 'state.tar'
        deployment.run('age', '--decrypt', '--identity', str(args.identity), '--output', str(archive), str(args.archive))
        if archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError('Decrypted archive exceeds the rehearsal size limit')
        stage = temporary / 'payload'
        stage.mkdir(mode=0o700)
        metadata, state = unpack_verified_archive(archive, stage, manifest)
        # All decryption/inventory/hash/SQLite/package checks precede mutation.
        fresh_or_resumable(args.sha256, manifest)
        if service_active():
            deployment.run('systemctl', 'stop', 'mumble-server.service')
        with tempfile.TemporaryDirectory(prefix='mumble-package-', dir='/var/tmp') as directory:
            os.chmod(directory, 0o755)
            package = Path(directory) / 'mumble-server.deb'
            shutil.copyfile(stage / 'mumble-server.deb', package)
            package.chmod(0o644)
            deployment.run('apt-get', 'update')
            deployment.run('apt-get', 'install', '-y', str(package), 'openssl', 'nftables', 'python3', 'age')
        uid = int(deployment.run('id', '-u', '_mumble-server'))
        gid = grp.getgrnam('_mumble-server').gr_gid
        deployment.DATA.mkdir(mode=0o750, exist_ok=True)
        os.chown(deployment.DATA, uid, gid)
        for suffix in ('-wal', '-shm'):
            (deployment.DATA / ('server.sqlite' + suffix)).unlink(missing_ok=True)
        for name, destination in PAYLOADS.items():
            if name == 'deployment.json':
                continue  # Keep the resumable ownership marker until ready.
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(stage / name, destination)
            mode = (0o640 if name in ('server.ini', 'server.crt', 'server.key') else
                    0o644 if name in ('hosting.conf', 'firewall.service') else 0o600)
            destination.chmod(mode)
            os.chown(destination, uid if name == 'server.sqlite' else 0,
                     gid if name in ('server.sqlite', 'server.ini', 'server.crt', 'server.key') else 0)
        deployment.run('nft', '--check', '-f', str(deployment.FIREWALL))
        deployment.run('systemctl', 'daemon-reload')
        deployment.run('systemctl', 'enable', '--now', 'mumble-hosting-firewall.service', 'mumble-server.service')
        time.sleep(1)
        deployment.run('systemctl', 'is-active', 'mumble-server.service')
        deployment.save(state)
    return {'status': 'restored', 'source_commit': manifest['source_commit'],
            'database_schema': metadata['database_schema'], 'seconds': round(time.monotonic() - started, 3),
            'external_readiness_required': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest='command', required=True)
    export = subcommands.add_parser('backup')
    export.add_argument('--recipient', required=True)
    export.add_argument('--output', type=Path, required=True)
    recover = subcommands.add_parser('restore')
    recover.add_argument('--archive', type=Path, required=True)
    recover.add_argument('--sha256', required=True, help='Trusted ciphertext digest from the backup record')
    recover.add_argument('--identity', type=Path, required=True)
    recover.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run as root inside the designated managed/disposable guest')
    os.umask(0o077)
    try:
        with Path('/run/lock/mumble-hosting.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            print(json.dumps(backup(args) if args.command == 'backup' else restore(args)))
    except (ValueError, OSError, RuntimeError, sqlite3.DatabaseError, tarfile.TarError,
            subprocess.TimeoutExpired) as error:
        print(f'State operation stopped: {error.__class__.__name__}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
