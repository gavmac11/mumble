"""Regression checks for unit accounting and invalid hosting scenarios."""

import unittest

import capacity_model
import collect_metrics
import relay_probe
import opus_fixture


class CapacityTests(unittest.TestCase):
    def scenario(self, *args):
        return capacity_model.calculate(capacity_model.parser().parse_args(args))

    def test_default_economics_is_explicitly_unvalidated(self):
        result = self.scenario()
        self.assertEqual(result["economics"]["break_even_customers"], 22)
        self.assertEqual(result["economics"]["monthly_contribution"], 139.2144)
        self.assertIsNone(result["resources"]["validated_sellable_capacity"])
        self.assertEqual(result["network"]["egress_with_allowance_mbps"], 434.7)

    def test_correlated_load_can_exceed_network_with_same_customer_count(self):
        result = self.scenario("--screen-groups", "36", "--webcam-groups", "4")
        self.assertFalse(result["network"]["within_budget"])
        self.assertEqual(result["network"]["egress_with_allowance_mbps"], 1366.2)

    def test_spare_and_setup_costs_count(self):
        result = self.scenario("--spare-host-monthly", "104", "--setup-fee", "120")
        self.assertEqual(result["economics"]["fixed_monthly_cost"], 268)
        self.assertEqual(result["economics"]["break_even_customers"], 37)

    def test_impossible_active_count_rejected(self):
        with self.assertRaises(ValueError):
            self.scenario("--customers", "3")

    def test_unprofitable_unit_has_no_break_even(self):
        result = self.scenario("--support-per-customer", "20")
        self.assertIsNone(result["economics"]["break_even_customers"])


class CounterTests(unittest.TestCase):
    def test_cpu_guest_time_is_not_double_counted(self):
        before = collect_metrics.parse_cpu("cpu 100 0 100 800 0 0 0 0 50 0")["cpu"]
        after = collect_metrics.parse_cpu("cpu 150 0 150 900 0 0 0 0 75 0")["cpu"]
        self.assertEqual(collect_metrics.cpu_usage(before, after)["busy_percent"], 50)

    def test_steal_is_separate_from_executed_work(self):
        result = collect_metrics.cpu_usage((0,) * 8, (20, 0, 20, 40, 0, 0, 0, 20))
        self.assertEqual(result["busy_percent"], 40)
        self.assertEqual(result["steal_percent"], 20)

    def test_decreasing_iowait_interval_is_missing_not_fabricated(self):
        result = collect_metrics.cpu_usage((0, 0, 0, 0, 10, 0, 0, 0),
                                          (10, 0, 0, 10, 9, 0, 0, 0))
        self.assertIsNone(result)

    def test_network_rate_units_and_counter_reset(self):
        zero = collect_metrics.parse_network(
            "eth0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0")["eth0"]
        later = collect_metrics.parse_network(
            "eth0: 125000000 100 1 2 0 0 0 0 250000000 200 3 4 0 0 0 0")["eth0"]
        result = collect_metrics.network_rates(zero, later, 2)
        self.assertEqual(result["rx_mbps"], 500)
        self.assertEqual(result["tx_mbps"], 1000)
        self.assertEqual(result["tx_packets_per_second"], 100)
        self.assertEqual(result["rx_drop_delta"], 2)
        self.assertIsNone(collect_metrics.network_rates(later, zero, 2))

    def test_missing_memory_is_null_and_kibibytes_become_bytes(self):
        result = collect_metrics.memory_bytes("MemTotal: 1024 kB\nSwapTotal: 0 kB\n")
        self.assertEqual(result["MemTotal"], 1048576)
        self.assertIsNone(result["MemAvailable"])
        self.assertEqual(result["SwapTotal"], 0)


class RelayEncodingTests(unittest.TestCase):
    def test_version_and_binary_payload_round_trip(self):
        version = (1 << 48) | (7 << 32)
        data = (relay_probe.integer(5, version) +
                relay_probe.blob(8, bytes(range(256))) +
                relay_probe.integer(24, 1))
        self.assertEqual(relay_probe.fields(data),
                         {5: version, 8: bytes(range(256)), 24: 1})

    def test_truncated_and_oversized_fields_are_rejected(self):
        for data in (bytes([0]), bytes([128]) * 11, bytes([10, 5, 1])):
            with self.assertRaises(ValueError):
                relay_probe.fields(data)

    def test_empty_latency_set_does_not_report_zero(self):
        self.assertIsNone(relay_probe.percentiles([]))
        self.assertEqual(relay_probe.percentiles(list(range(1, 101)))["p95"], 95)

    def test_annexb_split_keeps_all_bytes_and_distinguishes_nal_types(self):
        aud = bytes([0, 0, 0, 1, 9, 240])
        idr = bytes([0, 0, 1, 101, 77])
        pframe = bytes([0, 0, 1, 65, 88])
        data = aud + idr + aud + pframe
        self.assertEqual(relay_probe.annexb_frames(data), [aud + idr, aud + pframe])
        with self.assertRaises(ValueError):
            relay_probe.annexb_frames(idr)

    def test_opus_toc_duration_handles_modes_and_multiple_frames(self):
        self.assertEqual(opus_fixture.packet_duration_ms(bytes([9 << 3])), 20)
        self.assertEqual(opus_fixture.packet_duration_ms(bytes([13 << 3])), 20)
        self.assertEqual(opus_fixture.packet_duration_ms(bytes([(18 << 3) | 1])), 20)
        self.assertEqual(opus_fixture.packet_duration_ms(bytes([(16 << 3) | 3, 8])), 20)
        with self.assertRaises(ValueError):
            opus_fixture.packet_duration_ms(bytes([3]))
        with self.assertRaises(ValueError):
            opus_fixture.packet_duration_ms(bytes([(19 << 3) | 3, 63]))

    def test_truncated_ogg_is_rejected(self):
        with self.assertRaises(ValueError):
            opus_fixture.read_packets(b"OggS")


if __name__ == "__main__":
    unittest.main()
