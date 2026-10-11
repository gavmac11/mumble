# Release acceptance record

Copy this record for each candidate. Unrun, skipped, failed and passed are distinct statuses. A green build is not a completed record.

## Candidate and environment

- Release/version/source SHA:
- Stable/preview build channel and `update-prereleases` value:
- Package download, SHA-256 and signing verification:
- Client OS/version/architecture/session:
- CPU/GPU, camera, audio devices, backend and driver versions:
- Server source/package SHA and effective configuration:
- Provider/region/guest/host resource limits:
- Network path, measured RTT, UDP/TCP mode and injected impairments:
- Tester and date:
- Raw logs/counters/crash artifacts (redacted):

## Installed-client checklist

Use fresh machines without a compiler or developer runtime. Save the installed version and observed backend, not just the intended build options.

| Scenario | Status | Evidence / defect |
| --- | --- | --- |
| Fresh install and first voice connection | Unrun | |
| Install as a standard user; normal elevation only where required | Unrun | |
| Upgrade from previous stable and previous preview, preserve settings/certificates/trust | Unrun | |
| Roll back to known-good package with preserved usable state | Unrun | |
| Uninstall/reinstall; explicit data retention/deletion choice | Unrun | |
| Windows packaged FFmpeg/audio plugins load without developer paths | Unrun | |
| macOS quarantined download passes signing/notarization and launches | Unrun | |
| macOS 15, 26 and 27: record each advertised major/current patch separately | Unrun | |
| Ubuntu dependencies on clean 24.04 GNOME Wayland | Unrun | |
| Ubuntu dependencies on clean 24.04 GNOME Xorg | Unrun | |
| Microphone/camera/screen permission allow, deny, cancel and regrant | Unrun | |
| Mic/speaker/camera unplug and default-device switch | Unrun | |
| Sleep/wake and Wi-Fi/network change during active call | Unrun | |
| Screen/window/camera capture start, source switch and stop | Unrun | |
| High-DPI/multiple monitors, resize, capture source disappears | Unrun | |
| WebM playback with audio; malformed/large media; seek and output-device change | Unrun | |
| File-transfer first identity setup, decline then explicit retry/unlock | Unrun | |
| File-transfer password, wrong password, trust mismatch, cancel and disk-full | Unrun | |
| Exact received file hash, safe destination handling and temporary-file cleanup | Unrun | |
| Channel switch/disconnect/exit during media or transfer | Unrun | |
| Server restart and restored voice/media/file capability | Unrun | |
| Update available/current/offline/rate-limited; manual installation and rollback | Unrun | |
| Stable client ignores previews; exact stable-configured signed artifact accepted | Unrun | |
| Opt-in diagnostics export excludes secrets, media and personal data | Unrun | |

## Directed interoperability

Every row needs actual GUI voice, screen, webcam and file send/receive results. Capture/permissions and playback must both be exercised. Mark excluded advertised capabilities with their documented reason, never a generic pass.

| Sender | Receiver | Voice | Screen | Webcam | File/hash | Evidence |
| --- | --- | --- | --- | --- | --- | --- |
| Windows | macOS | Unrun | Unrun | Unrun | Unrun | |
| Windows | Ubuntu | Unrun | Unrun | Unrun | Unrun | |
| macOS | Windows | Unrun | Unrun | Unrun | Unrun | |
| macOS | Ubuntu | Unrun | Unrun | Unrun | Unrun | |
| Ubuntu | Windows | Unrun | Unrun | Unrun | Unrun | |
| Ubuntu | macOS | Unrun | Unrun | Unrun | Unrun | |

Include Ubuntu Wayland and Xorg capture cases, current/previous fork pairs, mixed upstream/fork rooms and unsupported feature refusal. Test each supported client against current and previous server candidates. Record capabilities rather than assuming all old/new combinations support new media.

## Workload and recovery results

Run on external endpoints as well as a controlled reference path. Collect both client and server observations. Packet relay timing is not audio playout timing. Preserve sender-offered, accepted, submitted and receiver-complete counts separately.

| Workload | Clients/senders | Transport / config | Duration | Voice quality / delay | Complete video / file hashes | Resource peaks / failures |
| --- | --- | --- | --- | --- | --- | --- |
| Voice only | 5, then 10 | UDP and forced TCP | | | | |
| One screen with voice | 10 / 1 screen | UDP and forced TCP | | | | |
| Two webcams with voice | 10 / 2 webcams | Unchanged default limits | | | | |
| Two webcams with voice | 10 / 2 webcams | Tested managed profile | | | | |
| All webcams with voice | 10 / 10 webcams | Tested managed profile | | | | |
| Screen plus webcams with voice | 10 / 1 screen + 9 webcams | Tested managed profile | 2 hours | | | |
| Concurrent large files and voice/video | Advertised maximum | Tested managed profile | | | | |
| Throttled receiver and legacy/mixed-capability room | Exact receiver mix recorded | UDP and TCP | | | | |
| Loss/jitter/bandwidth impairment | 1%, 3%, 5% loss separately | UDP and TCP; exact injection saved | | | | |
| Endurance and teardown cycles | Exact workload recorded | Candidate profile | 48 hours | | | |

## One-click service checklist

| Scenario | Status | Evidence / defect |
| --- | --- | --- |
| Fresh guest provisions pinned verified release and unique secrets | Unrun | |
| Repeating bootstrap is safe; partial failures reconcile | Unrun | |
| External authenticated TCP and UDP connectivity on assigned port | Unrun | |
| Reboot restarts service with retained state | Unrun | |
| Shared-IP TCP+UDP and SSH allocations persist and avoid collisions | Unrun | |
| Root tenant cannot reach management or another tenant | Unrun | |
| CPU/RAM/disk/network noisy tenant leaves adjacent voice usable | Unrun | |
| Consistent encrypted state export and customer download | Unrun | |
| Restore on fresh guest retains users, channels, ACLs, bans and TLS identity | Unrun | |
| Failed upgrade restores binary/config and compatible pre-migration DB | Unrun | |
| Credential/host identity reset and verified reimage before tenant reuse | Unrun | |
| Duplicate payment event creates one order/resource | Unrun | |
| Failed setup/refund cleans orphan resources without destroying another order | Unrun | |
| Expiry/cancellation releases every billable attachment; billing verified | Unrun | |
| Post-expiry state retention and deletion match published policy | Unrun | |
| Restore time and replacement capacity meet proposed recovery target | Unrun | |

## Decision

- Unresolved release blockers:
- Known limitations and user-facing documentation:
- Test skips and how native/hardware coverage was provided:
- Pilot contribution after support and all committed costs:
- Maintainer sign-off and distribution scope:
- Verdict: unqualified / internal candidate / private pilot / public stable.

Use the gates and dependencies in [the launch roadmap](stability-to-launch-roadmap.md). Attach new evidence after fixes; preserve earlier failures for comparison.
