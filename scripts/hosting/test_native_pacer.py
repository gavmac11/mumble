"""Run with MUMBLE_PILOT_PACER_LIBRARY pointing at the compiled queue bridge."""

import os
import unittest

from native_pacer import NativePacer


@unittest.skipUnless(os.environ.get("MUMBLE_PILOT_PACER_LIBRARY"), "native pacer not configured")
class PacerBridgeTests(unittest.TestCase):
    def setUp(self):
        self.pacer = NativePacer(os.environ["MUMBLE_PILOT_PACER_LIBRARY"], 2_400_000)

    def tearDown(self):
        self.pacer.close()

    def test_large_frames_above_the_old_bridge_limits_drain_completely(self):
        now = 0
        for count in (2049, 3000, 4096):
            with self.subTest(fragments=count):
                # Even 2049 full packets exceed the former 2 MiB bridge ceiling.
                packets = [index.to_bytes(4, "little") + b"p" * 1020 for index in range(count)]
                self.assertTrue(self.pacer.enqueue(packets, True, now))
                for packet in packets:
                    now += self.pacer.delay_ns(now)
                    self.assertEqual(self.pacer.take(now), packet)
                self.assertIsNone(self.pacer.take(now))
                self.assertIsNone(self.pacer.delay_ns(now))
                now += 1_000_000_000

    def test_out_of_bounds_frames_are_rejected_without_erasing_valid_work(self):
        self.assertTrue(self.pacer.enqueue([b"valid"], True, 0))
        for packets in ([b"x"] * 4097, [b"x" * 1025]):
            with self.assertRaisesRegex(ValueError, "invalid native pacer input"):
                self.pacer.enqueue(packets, True, 0)
        self.assertEqual(self.pacer.take(0), b"valid")


if __name__ == "__main__":
    unittest.main()
