"""Recovery fixture consumes only the connected user's server ban identifier."""
from types import SimpleNamespace
import unittest

from recovery_fixture import certificate_ban_identifier


class BanIdentifierTests(unittest.TestCase):
    def test_connected_user_identifier_is_preserved(self):
        identifier = b'0123456789abcdef' * 2 + b'01234567'
        peer = SimpleNamespace(session=7, users={7: {15: [identifier]}, 8: {15: [b'a' * 40]}})
        self.assertEqual(certificate_ban_identifier(peer), identifier.decode('ascii'))

    def test_missing_own_identifier_does_not_use_another_user(self):
        peer = SimpleNamespace(session=7, users={8: {15: [b'a' * 40]}})
        with self.assertRaisesRegex(AssertionError, 'valid certificate ban identifier'):
            certificate_ban_identifier(peer)

    def test_malformed_identifier_fails_closed(self):
        for identifier in (None, b'', b'a' * 39, b'a' * 41, b'g' * 40,
                           b'A' * 40, b'\x00' * 40, 'a' * 40):
            with self.subTest(identifier=identifier):
                peer = SimpleNamespace(session=7, users={7: {15: [identifier]}})
                with self.assertRaisesRegex(AssertionError, 'valid certificate ban identifier'):
                    certificate_ban_identifier(peer)


if __name__ == '__main__':
    unittest.main()
