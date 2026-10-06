#!/usr/bin/env python3
"""Calculate explicit hosting scenarios; does not benchmark or recommend density."""

import argparse
import json
import math


def nonnegative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return number


def positive(value):
    number = nonnegative(value)
    if number == 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def count(value):
    number = nonnegative(value)
    if not number.is_integer():
        raise argparse.ArgumentTypeError("must be a whole number")
    return int(number)


def percentage(value):
    number = nonnegative(value)
    if number >= 100:
        raise argparse.ArgumentTypeError("must be less than 100")
    return number


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--customers", type=count, default=40)
    cli.add_argument("--screen-groups", type=count, default=6)
    cli.add_argument("--webcam-groups", type=count, default=2)
    cli.add_argument("--mixed-groups", type=count, default=0)
    cli.add_argument("--host-monthly", type=nonnegative, default=104)
    cli.add_argument("--fixed-operations-monthly", type=nonnegative, default=50)
    cli.add_argument("--spare-host-monthly", type=nonnegative, default=0)
    cli.add_argument("--setup-fee", type=nonnegative,
                     help="Omitted means unknown and excluded, not free.")
    cli.add_argument("--setup-amortization-months", type=positive, default=12)
    cli.add_argument("--monthly-price", type=positive, default=9.99)
    cli.add_argument("--support-per-customer", type=nonnegative, default=2)
    cli.add_argument("--ip-per-customer", type=nonnegative, default=0)
    cli.add_argument("--payment-percent", type=percentage, default=3.6)
    cli.add_argument("--payment-fixed", type=nonnegative, default=0.30)
    cli.add_argument("--port-mbps", type=positive, default=1000)
    cli.add_argument("--network-headroom-percent", type=percentage, default=20)
    cli.add_argument("--traffic-allowance-percent", type=nonnegative, default=15)
    cli.add_argument("--ram-gib", type=positive, default=64)
    cli.add_argument("--host-reserve-gib", type=nonnegative, default=8)
    cli.add_argument("--guest-ram-gib", type=positive, default=1)
    cli.add_argument("--vm-overhead-gib", type=nonnegative, default=0.15)
    cli.add_argument("--usable-storage-gib", type=positive,
                     default=960 * 10**9 / 2**30,
                     help="Before headroom; default assumes two mirrored 960 GB disks.")
    cli.add_argument("--storage-headroom-percent", type=percentage, default=20)
    cli.add_argument("--guest-disk-gib", type=positive, default=15)
    cli.add_argument("--physical-cores", type=count, default=8)
    cli.add_argument("--vcpus-per-guest", type=count, default=1)
    return cli


def calculate(args):
    active = args.screen_groups + args.webcam_groups + args.mixed_groups
    if active > args.customers:
        raise ValueError("active communities cannot exceed paying customers in this scenario")
    if args.host_reserve_gib >= args.ram_gib:
        raise ValueError("host reserve must leave RAM for guests")
    if args.physical_cores == 0 or args.vcpus_per_guest == 0:
        raise ValueError("physical cores and vCPUs per guest must be positive")

    payload = 18 * args.screen_groups + 135 * args.webcam_groups + 139.5 * args.mixed_groups
    egress = payload * (1 + args.traffic_allowance_percent / 100)
    network_budget = args.port_mbps * (1 - args.network_headroom_percent / 100)
    ram_slots = math.floor((args.ram_gib - args.host_reserve_gib) /
                           (args.guest_ram_gib + args.vm_overhead_gib))
    disk_slots = math.floor(args.usable_storage_gib *
                            (1 - args.storage_headroom_percent / 100) / args.guest_disk_gib)
    after_fees = args.monthly_price * (1 - args.payment_percent / 100) - args.payment_fixed
    per_customer = after_fees - args.support_per_customer - args.ip_per_customer
    setup_monthly = (args.setup_fee or 0) / args.setup_amortization_months
    fixed = (args.host_monthly + args.fixed_operations_monthly +
             args.spare_host_monthly + setup_monthly)
    # Ceil after a small rounding tolerance prevents binary floating-point noise
    # from adding a customer at an exact mathematical break-even.
    break_even = math.ceil(fixed / per_customer - 1e-10) if per_customer > 0 else None
    warnings = [
        "Calculated scenario only: CPU, media quality, correlation, and production density are unmeasured.",
        "Media payload assumes ten people per active community at current target bitrates.",
        "Allowances are assumptions; cost excludes tax, refunds, overages and costs beyond entered reserves.",
    ]
    if args.setup_fee is None:
        warnings.append("Setup fee is unknown and excluded.")
    if args.spare_host_monthly:
        warnings.append("Spare cost includes only the entered amount; include its licensing and operations.")
    if egress > network_budget:
        warnings.append("Active media exceeds the network planning budget.")
    if args.customers > min(ram_slots, disk_slots):
        warnings.append("Sold quotas exceed at least one modeled RAM or disk budget.")
    if per_customer <= 0:
        warnings.append("Each added customer has nonpositive contribution before fixed costs.")

    return {
        "status": "unvalidated_scenario",
        "inputs": vars(args),
        "network": {
            "active_communities": active,
            "payload_mbps": round(payload, 4),
            "egress_with_allowance_mbps": round(egress, 4),
            "planning_budget_mbps": round(network_budget, 4),
            "within_budget": egress <= network_budget,
            "payload_decimal_gb_per_hour": round(payload * 0.45, 4),
        },
        "resources": {
            "ram_slots_only": ram_slots,
            "disk_slots_only": disk_slots,
            "allocated_vcpus_per_physical_core":
                args.customers * args.vcpus_per_guest / args.physical_cores,
            "validated_sellable_capacity": None,
        },
        "economics": {
            "currency": "USD (scenario inputs)",
            "after_payment_fees_per_customer": round(after_fees, 5),
            "contribution_per_customer_before_fixed_costs": round(per_customer, 5),
            "fixed_monthly_cost": round(fixed, 4),
            "break_even_customers": break_even,
            "monthly_contribution": round(args.customers * per_customer - fixed, 4),
        },
        "warnings": warnings,
    }


def main():
    cli = parser()
    args = cli.parse_args()
    try:
        result = calculate(args)
    except ValueError as error:
        cli.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
