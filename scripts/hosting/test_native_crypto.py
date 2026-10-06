"""Run with MUMBLE_PILOT_CRYPTO_LIBRARY pointing at the compiled native bridge."""

import os
import secrets
import unittest

from native_crypto import NativeCrypto


@unittest.skipUnless(os.environ.get("MUMBLE_PILOT_CRYPTO_LIBRARY"), "native bridge not configured")
class CryptoBridgeTests(unittest.TestCase):
    def setUp(self):
        key, left, right = (secrets.token_bytes(16) for _ in range(3))
        library = os.environ["MUMBLE_PILOT_CRYPTO_LIBRARY"]
        self.left = NativeCrypto(library, key, left, right)
        self.right = NativeCrypto(library, key, right, left)

    def tearDown(self):
        self.left.close()
        self.right.close()

    def test_maximum_packet_and_nonce_wrap(self):
        for _ in range(300):
            packet = bytes(range(256)) * 4
            self.assertEqual(self.right.decrypt(self.left.encrypt(packet)), packet)
        self.assertEqual(self.left.decrypt(self.right.encrypt(b"reply")), b"reply")

    def test_tamper_and_replay_are_rejected(self):
        encrypted = self.left.encrypt(b"message")
        damaged = bytearray(encrypted)
        damaged[-1] ^= 1
        self.assertIsNone(self.right.decrypt(bytes(damaged)))
        self.assertEqual(self.right.decrypt(encrypted), b"message")
        self.assertIsNone(self.right.decrypt(encrypted))

    def test_existing_xex_star_safeguard_changes_zero_block(self):
        # The unchanged repository intentionally flips a bit in this pattern.
        packet = b"P" * 64 + b"\x00" * 16 + b"P" * 7
        adjusted = bytearray(packet)
        adjusted[64] ^= 1
        self.assertEqual(self.right.decrypt(self.left.encrypt(packet)), bytes(adjusted))


if __name__ == "__main__":
    unittest.main()
