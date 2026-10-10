"""Recovery checks: real WAL snapshots, archive refusal, and age authentication."""
from contextlib import closing
import io
import json
from pathlib import Path
import sqlite3
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import deploy_guest
import guest_state


class StateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.stage = self.root / 'payload'
        self.stage.mkdir()
        self.manifest = {'schema': 1, 'source_commit': 'a' * 40, 'os': 'debian-12',
                         'architecture': 'amd64', 'package_version': '1.7.0~test',
                         'package_sha256': 'a' * 64}
        with closing(sqlite3.connect(self.stage / 'server.sqlite')) as database:
            database.execute('CREATE TABLE meta (meta_key TEXT, meta_value TEXT)')
            database.execute("INSERT INTO meta VALUES ('schema_version', '11')")
            database.execute('CREATE TABLE registered_users (id INTEGER, name TEXT)')
            database.execute("INSERT INTO registered_users VALUES (1, 'member')")
            database.commit()
        for name in guest_state.PAYLOADS:
            if name != 'server.sqlite':
                (self.stage / name).write_bytes(b'fixture')
        state = {'phase': 'ready', 'manifest': self.manifest,
                 'managed_files': {str(path): deploy_guest.digest(self.stage / name)
                                   for name, path in guest_state.PAYLOADS.items()
                                   if name not in ('server.sqlite', 'deployment.json', 'mumble-server.deb')}}
        (self.stage / 'deployment.json').write_text(json.dumps(state))
        self.metadata = {'schema': 1, 'package_manifest': self.manifest, 'database_schema': 11,
                         'files': {name: {'sha256': deploy_guest.digest(self.stage / name),
                                          'bytes': (self.stage / name).stat().st_size}
                                   for name in guest_state.PAYLOADS}}
        (self.stage / 'backup.json').write_text(json.dumps(self.metadata))

    def archive(self, name='state.tar', substitute=None, duplicate=None):
        output = self.root / name
        with tarfile.open(output, 'w') as stream:
            for filename in [*guest_state.PAYLOADS, 'backup.json']:
                if substitute and filename == substitute.name:
                    stream.addfile(substitute)
                else:
                    stream.add(self.stage / filename, arcname=filename)
            if duplicate:
                stream.add(self.stage / duplicate, arcname=duplicate)
        return output

    def unpack(self, archive):
        target = self.root / 'restore'
        target.mkdir(exist_ok=True)
        with patch.object(deploy_guest, 'validate_manifest'):
            return guest_state.unpack_verified_archive(archive, target, self.manifest)

    def test_sqlite_backup_includes_committed_wal_state(self):
        source = self.root / 'live.sqlite'
        with closing(sqlite3.connect(source)) as database:
            database.execute('PRAGMA journal_mode=WAL')
            database.execute('CREATE TABLE meta (meta_key TEXT, meta_value TEXT)')
            database.execute("INSERT INTO meta VALUES ('schema_version', '11')")
            database.execute('CREATE TABLE accounts (name TEXT)')
            database.execute("INSERT INTO accounts VALUES ('written-to-wal')")
            database.commit()
            self.assertTrue(Path(str(source) + '-wal').exists())
            target = self.root / 'snapshot.sqlite'
            self.assertEqual(guest_state.snapshot_database(source, target), 11)
            with closing(sqlite3.connect(target)) as snapshot:
                self.assertEqual(snapshot.execute('SELECT name FROM accounts').fetchall(), [('written-to-wal',)])

    def test_valid_inventory_and_database_are_accepted(self):
        metadata, state = self.unpack(self.archive())
        self.assertEqual(metadata['database_schema'], 11)
        self.assertEqual(state['phase'], 'ready')

    def test_path_traversal_is_rejected_before_extraction(self):
        archive = self.archive()
        with tarfile.open(archive, 'a') as stream:
            member = tarfile.TarInfo('../escape')
            member.size = 4
            stream.addfile(member, io.BytesIO(b'evil'))
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.unpack(archive)
        self.assertFalse((self.root / 'escape').exists())
        self.assertEqual(list((self.root / 'restore').iterdir()), [])

    def test_symlinks_are_rejected_before_extraction(self):
        member = tarfile.TarInfo('server.key')
        member.type = tarfile.SYMTYPE
        member.linkname = '/unrelated/key'
        with self.assertRaisesRegex(ValueError, 'non-regular'):
            self.unpack(self.archive(substitute=member))
        self.assertEqual(list((self.root / 'restore').iterdir()), [])

    def test_duplicate_payloads_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.unpack(self.archive(duplicate='server.ini'))

    def test_modified_payload_is_rejected(self):
        (self.stage / 'server.ini').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'digest/size'):
            self.unpack(self.archive())

    def test_schema_mismatch_is_rejected(self):
        self.metadata['database_schema'] = 12
        (self.stage / 'backup.json').write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError, 'schema differs'):
            self.unpack(self.archive())

    def test_owned_inventory_must_match_the_actual_payloads(self):
        state = json.loads((self.stage / 'deployment.json').read_text())
        state['managed_files']['/unrelated/file'] = 'a' * 64
        (self.stage / 'deployment.json').write_text(json.dumps(state))
        self.metadata['files']['deployment.json'] = {
            'sha256': deploy_guest.digest(self.stage / 'deployment.json'),
            'bytes': (self.stage / 'deployment.json').stat().st_size}
        (self.stage / 'backup.json').write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError, 'inventory is inconsistent'):
            self.unpack(self.archive())

    def test_failed_snapshot_restarts_the_previously_running_service(self):
        args = SimpleNamespace(output=self.root / 'backup.age',
                               recipient='age1' + 'a' * 58)
        paths = {name: self.stage / name for name in guest_state.PAYLOADS}
        with patch.object(guest_state, 'PAYLOADS', paths), \
                patch.object(guest_state, 'SPOOL_ROOT', self.root), \
                patch.object(guest_state, 'managed_state', return_value={'manifest': self.manifest}), \
                patch.object(guest_state, 'service_active', return_value=True), \
                patch.object(guest_state, 'snapshot_database', side_effect=ValueError('snapshot failed')), \
                patch.object(deploy_guest, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'snapshot failed'):
                guest_state.backup(args)
            self.assertEqual([call.args for call in command.call_args_list], [
                ('systemctl', 'stop', 'mumble-server.service'),
                ('systemctl', 'start', 'mumble-server.service'),
                ('systemctl', 'is-active', 'mumble-server.service')])
        self.assertFalse(args.output.exists())

    def key(self, name):
        identity = self.root / name
        subprocess.run(['age-keygen', '-o', str(identity)], check=True, capture_output=True)
        recipient = subprocess.check_output(['age-keygen', '-y', str(identity)], text=True).strip()
        return identity, recipient

    def test_age_round_trip_preserves_the_archive(self):
        identity, recipient = self.key('identity')
        archive = self.archive()
        encrypted = self.root / 'backup.age'
        guest_state.encrypt_archive(archive, recipient, encrypted)
        decoded = self.root / 'decoded.tar'
        deploy_guest.run('age', '-d', '-i', str(identity), '-o', str(decoded), str(encrypted))
        self.assertEqual(decoded.read_bytes(), archive.read_bytes())
        self.assertEqual(encrypted.stat().st_mode & 0o777, 0o600)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            guest_state.encrypt_archive(archive, recipient, encrypted)

    def test_wrong_key_and_authenticated_tamper_fail_before_guest_mutation(self):
        identity, recipient = self.key('identity')
        other, _ = self.key('other')
        encrypted = self.root / 'backup.age'
        guest_state.encrypt_archive(self.archive(), recipient, encrypted)
        manifest = self.root / 'manifest.json'
        manifest.write_text(json.dumps(self.manifest))
        changed = self.root / 'tampered.age'
        data = bytearray(encrypted.read_bytes())
        data[len(data) // 2] ^= 1
        changed.write_bytes(data)
        for ciphertext, key in [(encrypted, other), (changed, identity)]:
            args = SimpleNamespace(archive=ciphertext, sha256=deploy_guest.digest(ciphertext),
                                   identity=key, manifest=manifest)
            with self.subTest(file=ciphertext.name), \
                    patch.object(guest_state, 'SPOOL_ROOT', self.root), \
                    patch.object(guest_state, 'fresh_or_resumable') as mutation:
                with self.assertRaisesRegex(RuntimeError, 'age failed'):
                    guest_state.restore(args)
                mutation.assert_not_called()

    def test_wrong_ciphertext_pin_fails_before_decryption(self):
        ciphertext = self.root / 'backup.age'
        ciphertext.write_bytes(b'changed archive')
        with patch.object(deploy_guest, 'run') as decrypt, \
                patch.object(guest_state, 'fresh_or_resumable') as mutation:
            with self.assertRaisesRegex(ValueError, 'trusted backup pin'):
                guest_state.restore(SimpleNamespace(archive=ciphertext, sha256='0' * 64))
            decrypt.assert_not_called()
            mutation.assert_not_called()


if __name__ == '__main__':
    unittest.main()
