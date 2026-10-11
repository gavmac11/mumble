"""Checks that unsafe package pins are rejected before any installation."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import deploy_guest


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.package = Path(self.directory.name) / 'server.deb'
        self.package.write_bytes(b'candidate bytes')
        self.manifest = {'schema': 1, 'source_commit': 'a' * 40,
                         'package_sha256': hashlib.sha256(b'candidate bytes').hexdigest(),
                         'package_version': '1.7.0~preview+git12345678',
                         'os': 'debian-12', 'architecture': 'amd64'}
        self.metadata = '\n'.join(['Package: mumble-server',
                                   'Version: ' + self.manifest['package_version'],
                                   'Architecture: amd64'])

    def test_tampered_package_is_rejected_before_external_commands(self):
        self.package.write_bytes(b'changed bytes')
        with patch.object(deploy_guest, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'digest mismatch'):
                deploy_guest.validate_manifest(self.manifest, self.package)
            command.assert_not_called()

    def test_metadata_must_match_the_package_pin(self):
        for metadata in [self.metadata.replace('amd64', 'arm64'),
                         self.metadata.replace('mumble-server', 'mumble'),
                         self.metadata.replace('1.7.0', '1.8.0')]:
            with self.subTest(metadata=metadata), patch.object(deploy_guest, 'run', return_value=metadata):
                with self.assertRaisesRegex(ValueError, 'metadata'):
                    deploy_guest.validate_manifest(self.manifest, self.package)

    def test_partial_commit_or_wrong_guest_are_rejected(self):
        for field, value in [('source_commit', 'a' * 8), ('os', 'ubuntu-24.04'),
                             ('architecture', 'arm64'), ('package_sha256', 'bad'), ('schema', 2)]:
            with self.subTest(field=field), patch.object(deploy_guest, 'run') as command:
                with self.assertRaises(ValueError):
                    deploy_guest.validate_manifest({**self.manifest, field: value}, self.package)
                command.assert_not_called()

    def test_verified_package_metadata_is_accepted(self):
        with patch.object(deploy_guest, 'run', return_value=self.metadata):
            deploy_guest.validate_manifest(self.manifest, self.package)


if __name__ == '__main__':
    unittest.main()
