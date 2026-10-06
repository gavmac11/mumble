#!/usr/bin/env python3
"""Publish a complete, versioned preview. Reruns may repair drafts, never published releases."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


ASSETS = (
    "Mumble-Windows-x64.exe",
    "Mumble-macOS-arm64.zip",
    "Mumble-Ubuntu-24.04-amd64.deb",
    "Mumble-Debian-12-amd64.deb",
)


def gh(*args, missing=None):
    result = subprocess.run(["gh", *args], text=True, capture_output=True)
    if result.returncode:
        if missing and missing in result.stderr:
            return None
        raise RuntimeError(result.stderr.strip())
    return result.stdout.strip()


def publish(repo, sha, version, dist, run_url):
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("A full commit SHA is required")
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("Expected a three-component numeric release version")
    if any(int(component) > 65535 for component in version.split(".")):
        raise ValueError("Release version exceeds the installer/protocol limit; bump the version series")

    tag = f"v{version}"
    dist = Path(dist)
    hashes = []
    for name in ASSETS:
        asset = dist / name
        if not asset.is_file() or asset.stat().st_size == 0:
            raise ValueError(f"Missing or empty release asset: {name}")
        digest = hashlib.sha256()
        with asset.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        hashes.append(f"{digest.hexdigest()}  {name}\n")
    (dist / "SHA256SUMS.txt").write_text("".join(hashes))

    # Verify the exact commit before touching a release. Never move an existing tag.
    ref = gh("api", f"repos/{repo}/git/ref/tags/{tag}", missing="HTTP 404")
    if ref is not None:
        ref = json.loads(ref)["object"]
        if ref["type"] != "commit" or ref["sha"] != sha:
            raise ValueError(f"{tag} already points to a different commit or an annotated tag")

    release = gh("release", "view", tag, "--repo", repo,
                 "--json", "isDraft,targetCommitish", missing="release not found")
    if release is not None:
        release = json.loads(release)
        if not release["isDraft"]:
            if ref is None:
                raise ValueError("Published release has no matching source tag")
            print(f"{tag} is already published; leaving its assets unchanged")
            return
        if release["targetCommitish"] != sha:
            raise ValueError("Existing draft targets a different commit")

    if ref is None:
        gh("api", "--method", "POST", f"repos/{repo}/git/refs",
           "-f", f"ref=refs/tags/{tag}", "-f", f"sha={sha}")

    notes = dist / "release-notes.md"
    notes.write_text(
        f"Preview **{version}** of `master` at [`{sha[:8]}`](https://github.com/{repo}/commit/{sha}).\n\n"
        f"[Build and package checks]({run_url}).\n\n"
        "Download the package for your platform below. Close Mumble before installing; "
        "your existing settings are preserved. Verify downloads with `SHA256SUMS.txt`.\n\n"
        "These previews may be unstable. The Windows installer is unsigned. The macOS ARM64 "
        "application requires macOS 15 or newer and is ad-hoc signed, not notarized. "
        "The Ubuntu 24.04 amd64 package contains the client; the Debian 12 amd64 package contains the server.\n"
    )
    if release is None:
        gh("release", "create", tag, "--repo", repo, "--verify-tag", "--target", sha,
           "--title", f"Mumble {version} preview", "--notes-file", str(notes),
           "--generate-notes", "--draft", "--prerelease", "--latest=false")
    # Drafts are invisible to unauthenticated clients until every upload succeeds.
    gh("release", "upload", tag, *(str(dist / name) for name in (*ASSETS, "SHA256SUMS.txt")),
       "--repo", repo, "--clobber")
    gh("release", "edit", tag, "--repo", repo, "--draft=false", "--prerelease", "--latest=false")
    print(f"Published https://github.com/{repo}/releases/tag/{tag}")


if __name__ == "__main__":
    base = subprocess.check_output(["python3", "scripts/mumble-version.py"], text=True).strip()
    publish(os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_SHA"],
            f"{base}.{os.environ['GITHUB_RUN_NUMBER']}", "dist",
            f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}")
