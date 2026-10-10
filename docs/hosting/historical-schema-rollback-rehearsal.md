# Historical schema migration and rollback rehearsal

On 2026-10-10 a separate loopback-only database/service in the existing disposable Debian 12 guest exercised **schema 7 → 11 → 7**. The managed current deployment remained active on its original port. This follows the current-schema recovery rehearsal and adds evidence for a real older layout.

The old package was Debian bookworm's `mumble-server 1.3.4-4`, retrieved through signed APT metadata and verified against SHA256 `c7381408f4c2078844156ce7c38041648f7012ffc8eb0525e7855f7040966d5e`. Its binary was extracted into the private lab directory, with Qt5 runtime dependencies installed separately. The running managed package was not replaced. The upgrade used the previously pinned D1/D2 package `1.7.56~master+git984d6674`, SHA256 `c77f5c70c609fe6303d151f69f49821cd078a1d21bd8f21c73e4a5d1270c1974`, source `984d6674e8aa3304d4d9222ebaba5a408f584588`.

The old server needs `dbDriver=QSQLITE`; the newer server uses `sqlite`. The old SQLite metadata reports `version=7`, while the new layout reports `schema_version=11`. Reusing the current-layout driver/metadata assumptions failed during preparation and was corrected before accepted migration evidence was collected.

The fixture created a permanent room, certificate-registered member, group and ACLs, plus a certificate ban. Before the upgrade, external TLS/protocol checks verified administrator credentials, TLS identity, registered certificate login, authorized member entry, denied outsider entry, group/ACL values and ban behavior. The old service was stopped, SQLite's backup API captured the consistent pre-upgrade database, and a private snapshot retained that database, the matching INI/TLS identity and the exact older package with file hashes.

The pinned new server migrated the working database to schema 11. Database integrity and all external state checks passed. Rollback stopped that service, retained a diagnostic copy of the upgraded database, restored the saved older database/configuration/TLS bytes and started the corresponding older binary. It did not attempt to open the upgraded database with an older binary. Restored hashes matched before startup, schema 7 was confirmed, and all external state checks passed again.

The measured interval from initiating the upgrade-service stop through external post-rollback state verification was **1.617 seconds**. It includes SSH round trips, local snapshot copy, old-server startup and protocol checks. It excludes decryption, package/runtime installation, guest replacement and provider allocation. This is a local rehearsal measurement, not a customer RTO.

The legacy Qt5 server can send authentication/ServerSync packets after requesting a banned socket's disconnect. The first seed check exposed that timing; the accepted checks wait for actual socket closure and record whether ServerSync appeared first. It appeared in the post-rollback check and the connection then closed. Existing [upstream 1.3.4 code](https://raw.githubusercontent.com/mumble-voip/mumble/1.3.4/src/murmur/Server.cpp) requests disconnect during its encrypted callback. The fixture uses the older-compatible BanList message with a nonmatching documentation IP and the actual certificate hash, avoiding an IP ban on all loopback clients. This evidence does not claim that the old server rejects authentication before emitting any packet.

See the [validation record](results/2026-10-10/schema-rollback-validation.json), [pre-upgrade checks](results/2026-10-10/schema-preupgrade-check.json), [post-upgrade checks](results/2026-10-10/schema-postupgrade-check.json), [post-rollback checks](results/2026-10-10/schema-postrollback-check.json), [restored file checks](results/2026-10-10/schema-rollback-files.json) and [timing](results/2026-10-10/schema-rollback-time.json).

## Encrypted historical recovery

The pre-upgrade snapshot now also passes encrypted export and restoration onto a fresh Debian 12 amd64 guest. The dedicated [`historical_recovery.py`](../../scripts/hosting/historical_recovery.py) profile pins Debian's exact 1.3.4 package, schema 7, a fixed payload inventory and the explicit private rehearsal configuration. It refuses upgraded databases, unknown configuration keys, links, duplicate archive entries, package/payload mismatches and existing deployments. The schema-11 managed recovery tool is unchanged.

Export uses the operator's public age recipient. The encrypted archive was copied off the source guest; restore verified the operator's trusted ciphertext hash before decrypting, then verified every payload before changing server state. The older package is extracted into an owned runtime directory with separately installed Debian dependencies. A dedicated service account, systemd service, resource limits and firewall table serve the restored state. The original INI is retained privately; only database/TLS paths, listening address and port change in the generated INI. The database and TLS payload hashes match before startup.

The first uncommitted prototype failed: its restrictive file mask left the root directory at mode 0700, so the service account could not traverse it and systemd reported `200/CHDIR`. A transient active-state observation also incorrectly reported readiness. That disk is retained privately and its reported time is excluded from passing evidence. The final tool explicitly sets service directory permissions and requires a local TLS connection presenting the exact archived certificate before recording readiness. External state checks are still mandatory.

The accepted fresh restore took **53.938 seconds** for the operator invocation, including decryption, runtime dependency downloads/installation, verification and service/firewall setup through local TLS readiness. It excludes guest allocation/boot, age/Python bootstrap, archive transfer and the external checks. External checks verified administrator login, TLS identity, room, certificate registration, group/ACL behavior and banned connection closure. A real guest reboot changed its boot identity; both services stayed enabled and active, and all state checks passed again. This timing is a lab measurement, not a customer RTO.

A second fresh guest exercised actual reboot interruption after the database, runtime and service definitions were installed, immediately before service enable. A private fixture paused only that invocation boundary; production code has no pause hook. After reboot, the persistent marker remained `restoring`, the server was inactive, and the operator identity and decrypted tmpfs stage were gone. Supplying the key again and invoking the same pinned restore completed in **4.078 seconds**, with cached dependencies, and external state checks passed. This qualifies one reboot checkpoint; it does not qualify every interruption point or abrupt process death within the same boot.

Actual wrong-cipher-pin and wrong-age-key operations failed without creating server state. Attempting to restore over an already ready server failed without changing its PID, configuration, TLS identity or restore marker. The operator wrappers removed their temporary identities in `finally` blocks; **the recovery tool itself does not delete the caller's key**. Normally completed/erroring operations remove their plaintext staging directories; reboot cleared the interrupted tmpfs stage.

The public [`recovery_fixture.py`](../../scripts/hosting/recovery_fixture.py) now exposes `--legacy-ban` for the older BanList/socket-closure behavior. Its two-second total deadline also bounds a peer that keeps sending packets. Forced open-connection behavior failed as expected; modern mode still rejected a forced successful banned login. A timeout is handled before the broader `OSError` catch so it cannot count as a passed ban check. Current-schema state checks also passed with the default strict policy. The historical server can still emit ServerSync before closing, as described above.

There are **54 focused Python checks**, including 17 historical recovery checks, with no failures or skips. [CI run 38086044759](https://github.com/gavmac11/mumble/actions/runs/38086044759) passed those checks at recovery source `c41941e9971d3746006028e9c80825108199ba6f`; the later public fixture change is separately identified in the validation record. See the [encrypted recovery manifest](results/2026-10-10/historical-encrypted-recovery-validation.json), [fresh restore](results/2026-10-10/historical-fresh-recovery.json), [post-reboot state](results/2026-10-10/historical-postreboot-state.json), [interruption checkpoint](results/2026-10-10/historical-interruption-checkpoint.json), [post-interruption state](results/2026-10-10/historical-interruption-afterboot.json), [resumed restore](results/2026-10-10/historical-resumed-recovery.json), [resumed state checks](results/2026-10-10/historical-resumed-state.json) and [negative checks](results/2026-10-10/historical-encrypted-negatives.json). Archives, private identities, credentials and failed guest disks remain outside the repository.

For this explicit profile, run export inside the snapshot-owning guest with a public-recipient file, and restore only inside a designated fresh guest:

```sh
sudo python3 scripts/hosting/historical_recovery.py export \
  --snapshot /owned/pre-upgrade \
  --recipient "$(cat /run/operator-public-recipient.txt)" \
  --output /owned/historical-state.age

sudo python3 scripts/hosting/historical_recovery.py restore \
  --archive /owned/historical-state.age --sha256 TRUSTED_CIPHER_SHA256 \
  --identity /run/operator/recovery-identity --port 64738
```

Preserve and verify the export receipt independently of the archive, supply the identity temporarily and remove it afterward. For protocol checks, use `recovery_fixture.py --mode check --legacy-ban` with the archived fixture record, CA, server hostname and private test credentials. Local TLS readiness alone does not replace those external checks.

D2 remains open for additional interruption checkpoints, same-boot process death, durable off-host retention/key-loss drills, historical encrypted UDP/audio readiness and qualification of the frozen candidate on the chosen provider. This profile is a controlled historical rehearsal, not a generic production downgrade tool.
