# Private guest state recovery rehearsal

This is the first part of D2 in the [stability roadmap](stability-to-launch-roadmap.md), following the [D1 deployment rehearsal](guest-deployment-rehearsal.md). It adds an encrypted export and a restore into an isolated fresh Debian 12 amd64 systemd guest. It supports the managed SQLite layout and the exact package in the independently reviewed manifest. Schema migration/rollback, a frozen release candidate and commercial acceptance remain open.

## Export and recovery contract

Run `scripts/hosting/guest_state.py` as root **inside the designated guest**, with `deploy_guest.py` beside it and `age` installed. Do not run a tenant's recovery operation on the virtualization host. The source must be a ready deployment owned by the D1 installer, with unchanged managed configuration and its original pinned package still installed.

The export briefly stops a running server, uses SQLite's backup API with an integrity check, copies the managed config/TLS/credentials/package, then restarts the previously active service in a `finally` block. Stopped servers stay stopped. Snapshot/config failures still attempt service restart. A restart failure is reported and needs operator attention. Plaintext is staged in a private directory under `/run`; the source receives only the public age recipient. The operator keeps the private recovery identity separately from the tenant and exported archive.

The archive contains the SQLite snapshot, TLS certificate/private key, server INI, service limits, guest firewall/rules unit, credentials, installer ownership record and the pinned package. These are sensitive even though the final archive is encrypted. The rehearsal archive is bounded to 256 MiB; it does not support MySQL/PostgreSQL, arbitrary customer plugins/configuration, or a general VPS disk backup.

Restore requires three independently supplied inputs: ciphertext, its **trusted SHA-256 from the backup record**, and the reviewed package manifest. Encryption to a public recipient does not authenticate who created an archive. Never accept a digest supplied only alongside an untrusted uploaded archive. Review/distribute the backup record and package manifest through the operator's trusted channel.

Decryption, fixed archive inventory, regular-file checks, payload hashes/sizes, package metadata/pin and SQLite integrity/schema checks precede changes to the guest. Duplicate entries, links and traversal paths are rejected. Archive permissions and paths are never applied. Only the fixed owned paths are restored, with explicit guest ownership and modes. An existing deployment is refused. An interrupted operation can resume only from its own `restoring` marker and the same ciphertext/package pins.

The restore recreates the guest service/firewall and checks that the service is active. **External readiness is a separate required check**: successful `systemctl` output is insufficient. Restore does not silently change the database schema or substitute the currently newest package.

## Operator sequence

1. Generate an age identity on the operator machine and retain a separate recovery copy. Send only its public recipient to the source guest. In this drill the private identity never reached the source guest.
2. Export with `sudo python3 guest_state.py backup --recipient AGE_PUBLIC_RECIPIENT --output /private/state.age`. Retain the returned backup digest and reviewed package manifest through the trusted operator channel. Copy out the ciphertext; do not publish credentials, TLS private keys or the decrypted tar.
3. Prepare a fresh supported guest. Confirm `/run` is tmpfs for this rehearsal. Transfer the private identity directly to a private temporary directory there, for example `/run/mumble-restore-operator`, without a plaintext intermediate file on the virtualization host. Apply root ownership and mode `0600` before recovery.
4. Run `sudo python3 guest_state.py restore --archive /private/state.age --sha256 TRUSTED_DIGEST --identity /run/mumble-restore-operator/identity.txt --manifest reviewed-manifest.json`.
5. Remove the temporary identity immediately. Keep the operator's separate recovery identity. Test external administrator login, registered certificate login, allowed/denied channel entry, saved ACLs/groups/bans, pinned TLS identity and TCP/encrypted UDP relay. Reboot and repeat.
6. Keep the previous disk and immutable package/state pins until acceptance. For a schema-changing upgrade, roll back using the **pre-upgrade** backup and its matching binary/configuration in a replacement guest. Running an old binary against an already migrated DB is not an approved rollback.

For the protocol acceptance fixture, create private `member.crt/key` and `banned.crt/key` pairs and a mode-0700 secret directory containing those keys plus `join-password` and `admin-password`. `recovery_fixture.py --mode seed` creates the permanent room, certificate registration, group, ACL and certificate-only ban through the real server protocol. Retain its non-secret record, then run `--mode check` against the replacement using the original CA certificate and record. A member's remembered channel is deliberately left and re-entered to prove a new authorized move. The fixture is not a native desktop test.

## Recorded execution and remaining evidence

The [recovery manifest](results/2026-10-10/guest-recovery-validation.json) identifies the tool hashes, exact candidate package, environment, raw protocol records and timing. The source and replacement are real nested KVM Debian 12 guests on the supplied Ubuntu host. Existing host services and the older pilot were preserved.

The first restore command took 45.924 seconds. Subsequent external checks and a real guest reboot recovered the registered certificate account, permanent room, group/ACL behavior, certificate ban, credentials and original TLS identity. All seven managed file hashes matched. Server limits were 512 MiB, one CPU core, 128 tasks and 4096 open files. TCP and encrypted UDP fan-out were checked in separate runs; overlapping probes on the same channel contaminate each other's streams and are not acceptance evidence.

A second run preserved the recovered disk, reimaged that owned allocation from the cached cloud image, and recovered the same ciphertext/package into a blank disk. Measured from before powering down the old guest, external administrator/state checks passed at **211.098 seconds**, TCP relay at 215.870 seconds and encrypted UDP relay at **220.737 seconds**. Boot, package-download delay, prerequisite setup, direct key/ciphertext handoff and recovery are included. This is an observed local replacement drill of 3 minutes 41 seconds; cloud-image download, provider allocation and unavailable-host recovery are excluded. The second restore command itself took 50.954 seconds. These timings are not a production service promise.

The 12 recovery checks include a real committed-WAL snapshot, authenticated age round-trip/tamper/wrong-key refusal, fixed-inventory/path/link/duplicate refusal, payload/schema mismatch, managed inventory consistency and service restart after a failed snapshot. Together with the four deployment and 21 accounting/authentication checks, the focused suite has 37 tests. CI installs age and runs these suites without skipping encryption tests.

D2 remains open for the supported historical schema/version boundary and timed rollback. Scheduled/off-guest backup retention, recovery-key loss, interruption/resume and replacement-capacity failures need drills before a recovery promise. A small saved-state fixture and local nested-VM timings do not establish a production RPO/RTO, provider provisioning availability, media quality, physical-host density or profitable hosting.
