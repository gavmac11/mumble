# Private guest deployment rehearsal

This is the D1 workstream from [the stability foundation, PR 30](https://github.com/gavmac11/mumble/pull/30). `scripts/hosting/deploy_guest.py` installs one private preview on a **fresh Debian 12 amd64 systemd guest**. It is a rehearsal tool, not the public one-click product or a stable release installer. It deliberately refuses an existing server, changed owned configuration, changed package/version, or an automatic upgrade.

## Installation contract

Supply a local `.deb` and a separately reviewed JSON manifest. The manifest pins the complete actual build commit, package SHA-256, package version, distribution and architecture. A digest or metadata mismatch fails before installation. For pull-request artifacts, the actual checkout can be GitHub's synthetic merge commit; record that full commit from the package job, rather than substituting the PR branch head. The manifest is an operator trust input, not a signed release feed.

Example manifest shape (replace every example pin with verified candidate values):

```json
{
  "schema": 1,
  "source_commit": "0000000000000000000000000000000000000000",
  "package_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "package_version": "1.7.0~preview+git00000000",
  "os": "debian-12",
  "architecture": "amd64"
}
```

Run inside the designated disposable guest, after confirming that its SSH service uses the port supplied to the installer:

```sh
sudo python3 deploy_guest.py \
  --manifest candidate.json --package Mumble-Debian-12-amd64.deb \
  --endpoint preview.example.test --port 64738 --ssh-port 22
```

The installer retains its marker, package and private join/administrator credentials under `/var/lib/mumble-hosting` (root-only). It generates a persistent self-signed TLS identity, bootstraps the new database on loopback, reads the actual SuperUser server ID and verifies the stored PBKDF2 password. Passwords travel to the server through stdin, and command diagnostics are withheld from normal output. Bootstrap diagnostics remain in the root-only state directory. Operators must transfer credentials through their own authenticated private channel; this tool does not implement customer delivery or certificate issuance/rotation.

The server uses its packaged systemd unit and `_mumble-server` account. An owned override limits it to one CPU's quota, 512 MiB memory, 128 tasks and 4,096 file descriptors. An owned nftables table permits loopback, established traffic, ICMP, IPv4 DHCP replies, the declared SSH port and the paired Mumble TCP/UDP port. It does not flush other firewall tables. The guest must be fresh; cloud/provider ingress rules and port allocation remain separate work.

The private baseline has ten users and the existing **2.5 Mbps sender / 20 Mbps aggregate video defaults**. The [previous mixed-video comparison](pilot-default-limits-2026-10-04.md) failed at those defaults. These numbers are not a qualified paid media plan. Deployment readiness and media-quality capacity are different acceptance gates.

A second invocation with the same manifest/options preserves configuration, credentials and TLS identity. A root-only lock prevents simultaneous installation. Interrupted preparation can resume using its ownership marker; a changed ready deployment stops for an explicit migration/recovery procedure. External authenticated TCP/UDP and administrator-login checks are still required before treating an installation as ready.

## Rehearsal environment and evidence

The supplied Ubuntu 24.04 test host exposes KVM. We created dedicated, resource-limited QEMU runners in Docker, using a verified official Debian 12 generic cloud image (`20261006-2623`). Each actual guest has two virtual CPUs, 1,536 MiB RAM and an 8 GiB overlay. The runners use an unprivileged user, no container capabilities, an explicit KVM device, two CPU quota and a 3 GiB memory limit. Service ports are restricted to the host's private LAN address; management ports bind host loopback. Existing host services and the older pilot were preserved.

The package tested is the successful Debian artifact from [preview run 38077239024](https://github.com/gavmac11/mumble/actions/runs/38077239024), built at merge commit `984d6674e8aa3304d4d9222ebaba5a408f584588` (PR head `d78218004ece5e3d51b5cf0d40031ee70cf37c1c` merged into master `7fc94bf23cfb7e773282fdd67f30982e5bd25b33`). Package version: `1.7.56~master+git984d6674`; SHA-256: `c77f5c70c609fe6303d151f69f49821cd078a1d21bd8f21c73e4a5d1270c1974`. Later client-only channel-ID fixes do not change this artifact; this drill does not qualify a different package or frozen candidate.

See [deployment evidence](results/2026-10-10/guest-deployment-validation.json) and the neighboring external probe records. The first guest caught two rehearsal mistakes before acceptance: unsupported `sqlite_wal=1` and an assumed server ID of 1. The final tool uses WAL 2 and discovers/validates the actual account. Successful CLI exit alone did not prove administrator access; an actual external SuperUser TLS login did.

## Remaining release gates

Encrypted portable backups and fresh-guest restore of real accounts, ACLs, bans, configuration and TLS identity are D2. Timed rollback across the supported database-schema boundary, interrupted installation fault drills, upgraded-package behavior, automatic certificate rotation, customer credential delivery and provider ingress reconciliation remain unverified. Reboot of one nested guest is not a host-outage drill, multi-tenant isolation proof, physical-host density measurement or a service-level guarantee. Native GUI interoperability, signing, endurance and paid-pilot economics remain prerequisites for launch.
