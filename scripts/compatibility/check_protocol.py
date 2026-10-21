#!/usr/bin/env python3
"""Check additive protobuf and transport IDs against an official source tree.

Requires protoc and the Python protobuf package. No generated files are kept.
"""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

from google.protobuf import descriptor_pb2


PROTO_FILES = ("Mumble.proto", "MumbleUDP.proto")


def descriptors(root, scratch):
    # Skip missing files so that a removed file is reported as a failure instead of a protoc error.
    present = [name for name in PROTO_FILES if (root / "src" / name).is_file()]
    if not present:
        return {}
    output = scratch / "protocol.pb"
    subprocess.run([
        "protoc", f"--proto_path={root / 'src'}", f"--descriptor_set_out={output}", *present,
    ], check=True)
    result = descriptor_pb2.FileDescriptorSet()
    result.ParseFromString(output.read_bytes())
    return {file.name: file for file in result.file}


def check_messages(official, fork, prefix, failures, additions):
    current = {message.name: message for message in fork}
    for message in official:
        name = f"{prefix}.{message.name}"
        if message.name not in current:
            failures.append(f"Removed message {name}")
            continue
        candidate = current[message.name]
        by_name = {field.name: field for field in candidate.field}
        by_number = {field.number: field for field in candidate.field}
        for field in message.field:
            added = by_name.get(field.name)
            contract = lambda item: (item.number, item.type, item.type_name, item.label,
                                     item.default_value, item.HasField("oneof_index"), item.oneof_index)
            if added is None or contract(added) != contract(field):
                failures.append(f"Changed/removed field {name}.{field.name}")
            if field.number in by_number and by_number[field.number].name != field.name:
                failures.append(f"Reused field number {name}:{field.number}")
        old_names = {field.name for field in message.field}
        for field in candidate.field:
            if field.name not in old_names:
                additions.append(f"{name}.{field.name}={field.number}")
                if field.label == field.LABEL_REQUIRED:
                    failures.append(f"Required extension field {name}.{field.name}")
        check_enums(message.enum_type, candidate.enum_type, name, failures)
        check_messages(message.nested_type, candidate.nested_type, name, failures, additions)
    old_names = {message.name for message in official}
    for message in fork:
        if message.name not in old_names:
            additions.append(f"message {prefix}.{message.name}")
            for field in message.field:
                if field.label == field.LABEL_REQUIRED:
                    failures.append(f"Required extension field {prefix}.{message.name}.{field.name}")
            check_messages([], message.nested_type, f"{prefix}.{message.name}", failures, additions)


def check_enums(official, fork, prefix, failures):
    current = {enum.name: enum for enum in fork}
    for enum in official:
        candidate = current.get(enum.name)
        if candidate is None:
            failures.append(f"Removed enum {prefix}.{enum.name}")
            continue
        values = {value.name: value.number for value in candidate.value}
        for value in enum.value:
            if values.get(value.name) != value.number:
                failures.append(f"Changed enum {prefix}.{enum.name}.{value.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("official", type=Path)
    parser.add_argument("--fork", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    failures, additions = [], []
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        official, fork = descriptors(args.official, scratch), descriptors(args.fork, scratch)
        for name, original in official.items():
            candidate = fork.get(name)
            if candidate is None:
                failures.append(f"Removed proto file {name}")
                continue
            if (original.package, original.syntax) != (candidate.package, candidate.syntax):
                failures.append(f"Changed package/syntax: {name}")
            check_messages(original.message_type, candidate.message_type, original.package, failures, additions)
            check_enums(original.enum_type, candidate.enum_type, original.package, failures)
    ids = {}
    for transport in ("TCP", "UDP"):
        pattern = rf"PROCESS_MUMBLE_{transport}_MESSAGE\((\w+),\s*(\d+)\)"
        original = dict(re.findall(pattern, (args.official / "src/MumbleProtocol.h").read_text()))
        current = dict(re.findall(pattern, (args.fork / "src/MumbleProtocol.h").read_text()))
        for name, number in original.items():
            if current.get(name) != number:
                failures.append(f"Changed {transport} ID: {name}")
        if len(set(current.values())) != len(current):
            failures.append(f"Duplicate {transport} IDs")
        ids[transport] = {name: int(number) for name, number in current.items() if name not in original}
    print(json.dumps({"pass": not failures, "failures": failures, "additions": additions,
                      "transport_extensions": ids,
                      "official_protocol_md_exists": (args.official / "PROTOCOL.md").exists()}, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
