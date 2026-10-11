#!/usr/bin/env python3
"""Decode exported production dialog bitmaps and require complete binary safety data."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(directory):
    from PIL import Image
    import zxingcpp

    records = []
    for theme in ("light", "dark"):
        expected = (directory / f"{theme}.bin").read_bytes()
        if not expected or b"\0" not in expected:
            raise ValueError(f"{theme}: fixture must exercise embedded zero bytes")
        with Image.open(directory / f"{theme}.png") as image:
            barcode = zxingcpp.read_barcode(image, formats=zxingcpp.BarcodeFormat.QRCode)
        decoded = barcode.bytes if barcode else b""
        records.append({
            "theme": theme,
            "expected_bytes": len(expected),
            "decoded_bytes": len(decoded),
            "expected_sha256": hashlib.sha256(expected).hexdigest(),
            "decoded_sha256": hashlib.sha256(decoded).hexdigest(),
            "exact_match": decoded == expected,
        })
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--artifact", action="append", default=[])
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--revision", default="local")
    args = parser.parse_args()
    groups = {}
    try:
        for name in args.artifact or ["local"]:
            root = args.fixtures / name if args.artifact else args.fixtures
            directories = list(root.rglob("safety-qr")) if args.artifact else [root]
            if len(directories) != 1:
                raise ValueError(f"{name}: expected one safety-qr fixture directory, found {len(directories)}")
            groups[name] = verify(directories[0])
        passed = bool(groups) and all(row["exact_match"] for rows in groups.values() for row in rows)
        report = {"source_revision": args.revision, "passed": passed, "groups": groups}
    except (ImportError, OSError, ValueError) as error:
        report = {"source_revision": args.revision, "passed": False, "groups": groups, "error": str(error)}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
