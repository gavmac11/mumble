"""Version-bound historical archives: integrity, inventory, config and ownership."""
from contextlib import closing, ExitStack, contextmanager
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import deploy_guest
import historical_recovery as history


class HistoricalTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.payload = self.root / 'payload'
        self.payload.mkdir()
        with closing(sqlite3.connect(self.payload / 'server.sqlite')) as db:
            db.execute('CREATE TABLE meta (keystring TEXT PRIMARY KEY, value TEXT)')
            db.execute("INSERT INTO meta VALUES ('version', '7')")
            db.execute('CREATE TABLE accounts (name TEXT)')
            db.execute("INSERT INTO accounts VALUES ('saved-member')")
            db.commit()
        values = {'database': '/owned/server.sqlite', 'dbDriver': 'QSQLITE', 'sqlite_wal': '2',
                  'logfile': '', 'host': '127.0.0.1', 'port': '64739', 'users': '10',
                  'bandwidth': '72000', 'sslCert': '/owned/server.crt', 'sslKey': '/owned/server.key',
                  'serverpassword': 'a' * 48, 'allowping': 'false',
                  'autobanSuccessfulConnections': 'false', 'welcometext': 'Private fixture'}
        self.ini = values
        (self.payload / 'server.ini').write_text(''.join(f'{k}={v}\n' for k, v in values.items()))
        for name in ('server.crt', 'server.key', history.PACKAGE_NAME):
            (self.payload / name).write_bytes(b'fixture')
        self.package_hash = deploy_guest.digest(self.payload / history.PACKAGE_NAME)
        self.profile = {**history.PROFILE, 'package_sha256': self.package_hash}
        self.metadata()

    def metadata(self):
        self.record = {**self.profile, 'files': {
            name: {'sha256': deploy_guest.digest(self.payload / name),
                   'bytes': (self.payload / name).stat().st_size} for name in history.NAMES}}
        (self.payload / 'backup.json').write_text(json.dumps(self.record))

    def archive(self, replacement=None, extra=None):
        path = self.root / 'state.tar'
        with tarfile.open(path, 'w') as stream:
            for name in (*history.NAMES, 'backup.json'):
                if replacement and replacement.name == name:
                    stream.addfile(replacement)
                else:
                    stream.add(self.payload / name, arcname=name, recursive=False)
            if extra:
                stream.addfile(extra, io.BytesIO(b'x' * extra.size) if extra.isfile() else None)
        return path

    def unpack(self, archive):
        stage = self.root / 'restore'
        stage.mkdir()
        with patch.object(history, 'PACKAGE_SHA', self.package_hash), patch.object(history, 'PROFILE', self.profile):
            return history.unpack(archive, stage)

    def test_valid_version_bound_archive(self):
        self.assertEqual(self.unpack(self.archive())['database_schema'], 7)
        with closing(sqlite3.connect(self.root / 'restore/server.sqlite')) as db:
            self.assertEqual(db.execute('SELECT * FROM accounts').fetchall(), [('saved-member',)])

    def test_traversal_is_refused_before_any_payload_is_written(self):
        member = tarfile.TarInfo('../escape')
        member.size = 1
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.unpack(self.archive(extra=member))
        self.assertEqual(list((self.root / 'restore').iterdir()), [])
        self.assertFalse((self.root / 'escape').exists())

    def test_link_is_refused_before_any_payload_is_written(self):
        member = tarfile.TarInfo('server.key')
        member.type = tarfile.SYMTYPE
        member.linkname = '/unrelated/key'
        with self.assertRaisesRegex(ValueError, 'invalid payload'):
            self.unpack(self.archive(replacement=member))
        self.assertEqual(list((self.root / 'restore').iterdir()), [])

    def test_duplicate_name_is_refused(self):
        extra = tarfile.TarInfo('server.ini')
        extra.size = 1
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.unpack(self.archive(extra=extra))

    def test_payload_tampering_is_refused(self):
        (self.payload / 'server.key').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash/size'):
            self.unpack(self.archive())

    def test_different_package_pin_is_refused(self):
        record = self.record.copy()
        record['package_sha256'] = 'b' * 64
        (self.payload / 'backup.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'profile'):
            self.unpack(self.archive())

    def test_upgraded_database_is_refused_even_with_matching_payload_hash(self):
        with closing(sqlite3.connect(self.payload / 'server.sqlite')) as db:
            db.execute("UPDATE meta SET value='11'")
            db.commit()
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'schema-7'):
            self.unpack(self.archive())

    def test_unknown_config_is_refused_even_with_matching_payload_hash(self):
        with (self.payload / 'server.ini').open('a') as stream:
            stream.write('ice=tcp -h 0.0.0.0 -p 6502\n')
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            self.unpack(self.archive())

    def test_config_overrides_only_owned_destination_fields(self):
        settings = history.parse_ini(self.payload / 'server.ini')
        configured = dict(line.split('=', 1) for line in history.render_ini(settings, 64738).splitlines())
        for key in history.INI_KEYS - {'database', 'sslCert', 'sslKey', 'host', 'port'}:
            self.assertEqual(configured[key], settings[key])
        self.assertEqual(configured['port'], '64738')
        self.assertEqual(configured['host'], '0.0.0.0')
        self.assertEqual(settings, self.ini)
        with self.assertRaises(ValueError):
            history.render_ini(settings, 22)

    def test_local_readiness_requires_the_archived_tls_identity(self):
        certificate = self.payload / 'server.crt'
        secure = unittest.mock.MagicMock()
        secure.__enter__.return_value = secure
        secure.getpeercert.return_value = b'wrong-leaf'
        context = unittest.mock.MagicMock()
        context.wrap_socket.return_value = secure
        with patch.object(history.ssl, 'create_default_context', return_value=context), \
                patch.object(history.ssl, 'PEM_cert_to_DER_cert', return_value=b'archived-leaf'), \
                patch.object(history.socket, 'create_connection', return_value=unittest.mock.MagicMock()):
            with self.assertRaisesRegex(ValueError, 'different TLS identity'):
                history.local_tls_ready(64738, certificate)
            secure.getpeercert.return_value = b'archived-leaf'
            history.local_tls_ready(64738, certificate)
        self.assertFalse(context.check_hostname)

    def test_failed_listener_never_qualifies_readiness(self):
        with patch.object(history.ssl, 'create_default_context'), \
                patch.object(history.ssl, 'PEM_cert_to_DER_cert', return_value=b'leaf'), \
                patch.object(history.socket, 'create_connection', side_effect=ConnectionRefusedError), \
                patch.object(history.time, 'monotonic', side_effect=[0, 11]):
            with self.assertRaisesRegex(RuntimeError, 'did not become ready'):
                history.local_tls_ready(64738, self.payload / 'server.crt')

    @contextmanager
    def guest_context(self):
        directory = self.root / 'owned'
        original_read = Path.read_text
        original_dir = Path.is_dir
        def read(path, *args, **kwargs):
            if path == Path('/etc/os-release'):
                return 'ID=debian\nVERSION_ID="12"\n'
            return original_read(path, *args, **kwargs)
        def is_dir(path):
            return True if path == Path('/run/systemd/system') else original_dir(path)
        with ExitStack() as stack:
            for name, value in [('ROOT', directory), ('STATE', directory / 'state'),
                                ('DATA', directory / 'data'), ('UNIT', self.root / 'unit.service'),
                                ('FW_UNIT', self.root / 'firewall.service')]:
                stack.enter_context(patch.object(history, name, value))
            for name in ('STATE', 'DATA', 'INI'):
                stack.enter_context(patch.object(deploy_guest, name, self.root / ('other-' + name)))
            stack.enter_context(patch.object(Path, 'read_text', read))
            stack.enter_context(patch.object(Path, 'is_dir', is_dir))
            stack.enter_context(patch.object(deploy_guest, 'run', return_value='amd64'))
            stack.enter_context(patch.object(history.subprocess, 'run',
                                            return_value=SimpleNamespace(returncode=1, stdout='')))
            yield directory

    def test_private_umask_preserves_service_directory_traversal(self):
        with self.guest_context() as directory:
            previous = os.umask(0o077)
            try:
                history.fresh_or_owned('a' * 64, 64738)
            finally:
                os.umask(previous)
            self.assertEqual(directory.stat().st_mode & 0o777, 0o750)
            self.assertEqual((directory / 'state').stat().st_mode & 0o777, 0o700)
            marker = json.loads((directory / 'state/restore.json').read_text())
            self.assertEqual(marker['phase'], 'restoring')

    def test_owned_partial_restore_resumes_only_same_cipher_and_port(self):
        with self.guest_context() as directory:
            history.fresh_or_owned('a' * 64, 64738)
            before = (directory / 'state/restore.json').read_bytes()
            history.fresh_or_owned('a' * 64, 64738)
            self.assertEqual((directory / 'state/restore.json').read_bytes(), before)
            with self.assertRaisesRegex(ValueError, 'does not belong'):
                history.fresh_or_owned('b' * 64, 64738)
            with self.assertRaisesRegex(ValueError, 'does not belong'):
                history.fresh_or_owned('a' * 64, 64739)

    def test_completed_restore_is_never_overwritten(self):
        with self.guest_context() as directory:
            history.fresh_or_owned('a' * 64, 64738)
            marker = directory / 'state/restore.json'
            record = json.loads(marker.read_text())
            record['phase'] = 'ready'
            marker.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, 'does not belong'):
                history.fresh_or_owned('a' * 64, 64738)
            self.assertEqual(json.loads(marker.read_text())['phase'], 'ready')

    def test_preexisting_broken_unit_link_is_not_replaced(self):
        with self.guest_context() as directory:
            history.UNIT.symlink_to(self.root / 'unrelated-missing')
            with self.assertRaisesRegex(ValueError, 'links'):
                history.fresh_or_owned('a' * 64, 64738)
            self.assertTrue(history.UNIT.is_symlink())
            self.assertFalse(directory.exists())

    def test_wrong_ciphertext_pin_precedes_decryption_and_guest_mutation(self):
        archive = self.root / 'cipher.age'
        archive.write_bytes(b'not an age file')
        args = SimpleNamespace(archive=archive, sha256='a' * 64, identity=self.root / 'key', port=64738)
        with patch.object(history, 'fresh_or_owned') as own, patch.object(deploy_guest, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'operator pin'):
                history.restore(args)
            own.assert_not_called()
            run.assert_not_called()

    def test_decryption_failure_precedes_guest_mutation(self):
        archive = self.root / 'cipher.age'
        archive.write_bytes(b'not an age file')
        args = SimpleNamespace(archive=archive, sha256=deploy_guest.digest(archive),
                               identity=self.root / 'key', port=64738)
        original = tempfile.TemporaryDirectory
        with patch.object(history.tempfile, 'TemporaryDirectory',
                          side_effect=lambda **kw: original(dir=self.root)), \
                patch.object(history, 'fresh_or_owned') as own, \
                patch.object(deploy_guest, 'run', side_effect=RuntimeError('age failed')):
            with self.assertRaisesRegex(RuntimeError, 'age failed'):
                history.restore(args)
            own.assert_not_called()
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['cipher.age', 'payload'])


if __name__ == '__main__':
    unittest.main()
