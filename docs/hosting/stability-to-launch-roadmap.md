# ADR-001: Gate one-click hosting on cross-platform release evidence

Status: proposed execution plan. Date: October 10, 2026.
Baseline: `origin/master` at `7fc94bf23cfb7e773282fdd67f30982e5bd25b33`.
Deciders: project maintainer, platform owners, and hosting operations owner.

## Decision

Choose a release candidate, prove the installed clients work together, then automate a single supported server deployment. Open a private paid pilot only after recovery and costs have been measured. Gate public signup on repeatable client quality, infrastructure isolation, and positive contribution after support.

Continue the existing [bare-metal launch proposal](launch-research.md): one KVM guest per community, shared public IPv4 with separate Mumble/SSH ports, customer root access, portable community state, and optional cloud overflow. This roadmap supplies the client and release gates that proposal still needs. It does not establish a supplier, sellable density, or launch date.

For the first engineering milestone, one provider-managed VPS is sufficient to validate deployment and clients. That does not validate bare-metal economics. A customer-owned VPS installer can be a useful limited offering if people will pay for setup and maintenance; treat it as an option to validate, not a replacement for the existing hosting design.

Keep stabilization focused. Add changes required for reliable advertised behavior, security, observability, or recovery. Defer new codecs, platforms, elaborate billing, and automatic updater replacement. Hour/day rentals and annual commitments follow a working monthly lifecycle.

### Options considered and trade-offs

| Option | Complexity / cost | Advantage | Limitation | Decision |
| --- | --- | --- | --- | --- |
| Build public checkout on current previews | Medium engineering; high support exposure | Earliest sale opportunity | Known client defects and unvalidated recovery become customer incidents | Defer |
| Prove clients and one guest, then qualify the existing KVM hosting proposal | Medium initially, higher for isolated lifecycle/density; fixed supply commitments | Clear release evidence and potentially better unit economics at measured occupancy | Requires isolation, recovery, traffic and hardware qualification before supply commitment | Recommended sequence |
| Customer-owned VPS setup/maintenance first | Lower infrastructure commitment; support still significant | Tests demand without owning a media fleet | Customer environments vary; setup revenue alone may not fund maintenance | Optional demand experiment |
| One retail provider VPS per paid customer | Lower host-management complexity; higher variable supply cost | Useful small technical pilot and overflow path | Existing sub-$10 price is fragile after backups and support | Pilot/overflow comparison |

The principal consequence is a later public sale with fewer unmeasured operational assumptions. We accept work on signing, native acceptance and recovery before checkout. The bare-metal model remains conditional on measured occupancy/density; customer-owned deployment remains portable. Revisit this decision if measured support, safe density, or customer willingness to pay invalidate the proposed price and scope.

## What we can substantiate today

| Evidence | Consequence |
| --- | --- |
| Latest baseline CI, preview installers, and security workflows succeeded on October 6. [CI](https://github.com/gavmac11/mumble/actions/runs/37546634671), [previews](https://github.com/gavmac11/mumble/actions/runs/37546634596), [security](https://github.com/gavmac11/mumble/actions/runs/37546634691). | Buildability is established for their configurations. This is not an installed-client quality result. |
| [Build matrix](../../.github/workflows/build.yml) enables client tests on Ubuntu; Windows and Intel macOS set tests OFF. ARM macOS preview builds also inherit tests OFF. | Make client regression tests mandatory on the three shipped platforms. Intel build success alone does not create a supported Intel package. |
| [Preview pipeline](../../.github/workflows/preview-installers.yml) verifies packages, dependencies, and server tests. Its publication dependencies do not include the separate full client CI job. | A successful preview may publish without the complete client suite passing at the same SHA. Stable promotion must explicitly depend on all required checks. |
| [Update maintenance](../dev/ClientUpdates.md) documents versioned immutable previews, stable-channel filtering, rollback downloads, bounded update requests, and manual installation. [README](../../README.md) identifies Windows previews as unsigned and macOS previews as ad-hoc signed/unnotarized. | Reuse the release machinery. Sign Windows packages and sign/notarize macOS packages before broad consumer distribution; preserve manual updates initially. |
| [PR 29](https://github.com/gavmac11/mumble/pull/29) reports failing queued file-transfer sends and recovery after declining identity setup. Baseline [FileTransferManager.cpp](../../src/mumble/PQFileTransfer/engine/FileTransferManager.cpp) invokes send methods by string; [ServerHandler.h](../../src/mumble/ServerHandler.h) declares them as plain methods. | Treat this as a release blocker for advertised file sharing. PR validation is reported evidence; rerun the installed GUI path after review and merge. The current plan does not merge it. |
| [File-transfer E2E test](../../src/tests/TestFileTransferE2E/TestFileTransferE2E.cpp) uses real TLS client connections and engines but installs its own transport below the GUI. | The existing end-to-end test does not cover the broken manager-to-ServerHandler path. Add coverage at that production boundary. |
| [October 4 results](pilot-default-limits-2026-10-04.md) recorded 85,680/130,896 video deliveries at default 2.5/20 Mbps limits versus 130,896/130,896 at raised 3.5/200 Mbps, with all voice delivered in both cases. | Two webcams in a ten-user room already exceed the default aggregate video allowance. A one-click deployment must choose an explicit tested profile and disclose media limits. Raising limits alone does not prove capacity or fix feedback. |
| [Existing density runbook](host-density-pilot.md), [relay probe](../../scripts/hosting/relay_probe.py), [metrics collector](../../scripts/hosting/collect_metrics.py), and [capacity model](../../scripts/hosting/capacity_model.py) preserve useful measurements and distinguish client pacing from relay loss. | Reuse them. Repeat on the release candidate and actual intended supply; prior short container/VM experiments are not a physical-host density result. |
| Linux portal/PipeWire, V4L2, macOS ScreenCaptureKit/AVFoundation, video packetization/pacing, WebM, and file-transfer implementations and tests exist. | Focus on lifecycle, packaging, and integration evidence rather than rebuilding these components. |
| [Server service template](../../auxiliary_files/config_files/mumble-server.service.in) already runs under a dedicated user with restrictions and restart behavior; [Debian package script](../../.github/workflows/package_debian_server.sh) installs config and service assets. | Build deployment on this foundation. Package installation and `--version` do not prove boot startup, remote connection, upgrade, or restore. |

No fresh GUI session, WAN load run, supplier purchase, production deployment, or physical-host test was performed for this planning review. Your original checkout's unfinished audio and capture work remains separate; assess it against this baseline before choosing any portion for the candidate.

## Initial support contract

| Product | Initial supported target | Required coverage |
| --- | --- | --- |
| Windows client | Windows 11 x64; name exact supported OS releases in the release manifest | Fresh standard-user install, upgrade, audio/camera privacy controls, WASAPI/device changes, screen/window sharing, codec DLL loading, uninstall with data preservation choice. |
| macOS client | Apple Silicon; qualify macOS 15, 26 and 27 separately, with current patch releases recorded | Quarantined download, notarization, microphone/camera/screen consent including denial/regrant, device changes, sleep/wake, capture stop and app shutdown. |
| Ubuntu client | Ubuntu 24.04 LTS amd64, GNOME Wayland and GNOME Xorg | Package install on clean OS, PipeWire/portal permissions and cancellation, PulseAudio/PipeWire audio, V4L2 camera, X11 window selection, desktop changes. |
| Community server | Debian 12 amd64 technical baseline; choose production Debian 12 LTS or qualified Debian 13 before freezing deployment | Match the existing server package; fresh boot, TCP and UDP on assigned port, persistent state, upgrade and recovery. Ubuntu server support requires a separate package/runtime acceptance run. |

Platform lifecycle was checked against primary sources: Apple lists macOS 27.0.1 as current, alongside 26.7.1 and 15.8.1 updates; Debian 12 is superseded but has amd64 LTS through June 30, 2028, while Debian 13 is the newer release. Do not confuse the macOS 15 build runner/minimum with current-user coverage. Decide whether Debian 12 LTS covers the deployed package dependencies and maintenance horizon, or qualify Debian 13 packaging before choosing it. [Apple releases](https://support.apple.com/en-us/100100), [Debian 12 lifecycle](https://www.debian.org/releases/bookworm/), [Debian 13 lifecycle](https://www.debian.org/releases/trixie/).

Linux desktops beyond the named targets, Intel Mac installers, Windows ARM, mobile, and additional server distributions remain unqualified until the same gates pass. Keep existing Intel build coverage as a regression signal. Publish feature-level exceptions plainly if the release excludes a capability; hiding a control is insufficient unless the unsupported path is also handled safely.

Run all six directed transfers/streams between the three desktop platforms. Test current/current, current/previous fork, and fork/upstream voice interoperability. Video/files must negotiate or refuse unsupported operations visibly without affecting voice. Include a mixed-capability room: legacy clients must not be disconnected or flooded with unsupported media.

## Release gates

All thresholds below are proposed internal acceptance targets, not measurements or customer guarantees. Record exact binaries, source SHA, dependency versions, OS, audio/video hardware, server config, network conditions, and observed results.

1. **Required checks at one SHA.** Existing deterministic tests pass on Ubuntu, Windows x64, and macOS ARM64, plus server tests and publisher/update checks. Add production manager/thread transport coverage, capture lifecycle regression tests where automation is meaningful, and feature-disabled build checks. Linux ASan/UBSan nightly runs cover shutdown/reconnect/decoder and transfer teardown. Hardware-dependent checks must report skips explicitly and be covered on device-equipped machines.
2. **Installed artifact acceptance.** Download the exact signed candidate installers onto clean machines without developer dependencies. A stable client must be built with `-Dupdate-prereleases=OFF`; simply promoting a preview preserves its preview-channel behavior. Run required checks on the stable build configuration, then accept the final signed/notarized artifacts and record their hashes. Exercise first run, existing settings/certificates/trust data migration, upgrade from the previous release, rollback, and uninstall/reinstall. Verify loaded codec/audio plugins and the complete signed dependency bundle. Record a passing result for every advertised OS/session combination.
3. **Real media and transport.** Two-hour real GUI mixed-media run with 5 and 10 participants: voice-only, one screen, two webcams, all webcams, and screen plus remaining webcams. Exercise normal UDP and forced TCP fallback. Replay bounded reference media alongside actual speech/capture. Compare voice-only and mixed-media timing at the same network conditions; proposed target is no audible degradation and less than 20 ms increase in p95 voice relay delay. Probe delay is not mouth-to-ear latency: measure actual playout separately. A controlled reference run must complete all expected file hashes and video frames, with no unexplained missing deliveries.
4. **Recovery and adverse conditions.** Run permission denial/cancellation/regrant, camera/mic unplug, audio default switch, sleep/wake, network change, server restart, channel change mid-share/transfer, transfer decline/wrong password/cancel, and exit during capture/decode. Inject 1%, 3%, and 5% loss, jitter, and bandwidth caps. No crash, deadlock, unbounded queues, leaked capture, plaintext residue, or silent permanent failure. Proposed target: voice resumes within 15 seconds of a reachable connection; video recovers or explains refusal within 5 seconds after voice restoration. Document required user actions for OS consent/device recovery.
5. **Endurance.** At least 48 hours of sustained/repeated automated connections plus real GUI sessions, 100 capture start/stop cycles per supported capture path, and 100 reconnect cycles. No server OOM, application restart, or crash. Resource use returns near the warmed idle baseline after teardown (proposed tolerance 10% plus 50 MiB); investigate monotonic growth rather than assuming a fixed allowance proves no leak. Preserve failure logs and retest repaired cases.
6. **Media budget behavior.** Repeat both unchanged default and raised pilot templates; preserve the known failing default case. Add server per-sender/aggregate rejection counters and rate-limited, visible feedback. Start with one pinned named profile and clearly refuse unsupported media load; validate that behavior rather than assuming a notice fixes frame recovery. Full negotiated budgets/quality adaptation may follow when needed for dynamic allocation. Test custom low caps, changing participant counts, runtime changes, and legacy fallback. Protect voice when video/files saturate TCP or server egress, including a deliberately slow receiver. Qualify a bounded advertised media profile before offering unlimited webcam/screenshare claims.
7. **Release promotion.** Promote a frozen candidate after all gates pass; do not call every successful merge stable. Keep symbols privately available for crash analysis, immutable artifacts and checksums, release notes, tested recovery instructions, and feature compatibility notes. Internal users first, then a small opt-in cohort, then wider distribution. Revoke a bad stable promotion and offer a known-good signed installer; GitHub's highest-version checker cannot downgrade an already-installed client automatically.

Use opt-in, local-exportable diagnostics: version/build, OS, selected backend, consent state, queue/drop counters, anonymized failure codes and timings. Exclude microphone samples, captured pixels, file bytes/names, passwords, keys, and unredacted server/user identities by default. Tie every support report to a candidate and scenario.

## Implementation sequence

These are work packages and exit criteria, not calendar promises. Assign a named owner when scheduling; roles here identify the necessary responsibility.

| Order | Work package / owner role | Concrete result | Dependency |
| --- | --- | --- | --- |
| S1 | Candidate triage / maintainer | Review PR 29, validate its GUI behavior, assess unfinished local changes separately, list blockers and exact supported features. Freeze candidate SHA only after fixes land. | Latest baseline |
| S2 | Cross-platform regression CI / release engineer | Enable tests on Windows/ARM macOS; solve static Qt test-plugin packaging; gate promotion on all same-SHA checks; retain failure artifacts and symbols. | S1 for final candidate |
| S3 | Integration harness / client engineer | Cover real manager-to-ServerHandler delivery, receiver destruction, cancelled identity retry, reconnect capability announcement, media teardown. Record six directed platform pairs. | S1/S2 |
| S4 | Video budget and diagnostics / client + server engineer | One named tested profile, rejection counters and visible refusal; validate default and managed profiles including TCP congestion/slow receivers. Review legacy video routing and file-data fan-out. Keep full [budget negotiation](host-density-pilot.md#budget-negotiation-follow-up) as a follow-up where fixed limits/refusal suffice. | S1; required for advertised multi-camera profile |
| S5 | Native quality and packaging / platform owners | Run the acceptance record, fix platform defects, sign Windows and notarize macOS; preserve Ubuntu session and runtime dependency evidence. | S2–S4 |
| D1 | Reproducible server deployment / infrastructure engineer | One idempotent deployment on a fresh supported guest, pinned package and verified release identity, readiness probe from another network, restart after reboot, redacted failures. | Can prototype during S2; paid pilot waits for S5 |
| D2 | Community state and rollback / infrastructure engineer | Consistent encrypted off-host DB/config/TLS-state export; restore accounts/ACLs/bans/identity on a fresh guest, including version/schema rollback. Record restore time. | D1 |
| P0 | Friendly preview pilot / product + operations owner | 3–5 consenting communities on provider-managed VPSs, one profile/region, up to 30 days; measure support, video-hours, file traffic and egress before shared-host investment. No production promises. | Critical pilot-entry gates below; D1/D2 |
| H1 | Shared-IP guest lifecycle / infrastructure engineer | Persistent paired TCP/UDP plus SSH port allocation, isolated VM with externally enforced limits, root-tenant reimage, credential reset, billing/resource reconciliation. | D1/D2 |
| H2 | Supply and density qualification / operations owner | Written complete provider terms, correlated multi-VM load, noisy-neighbor isolation, real hardware headroom and failure recovery; admission limit below repeatedly passing density. Use P0 demand/cost evidence before supply commitment. | S5/H1/P0 |
| P1 | Small monthly paid pilot / product + operations owner | 5–10 communities, one qualified region/profile, 30 days; logged setup success, tickets/minutes, usage, restore drills, payments/refunds/expiry and realized contribution. | S5/D2/H1/H2 |
| P2 | Public one-click release / maintainer | All quality/economic gates pass; concise plan limits, standard-setup support boundary, cancellation/export and recovery policy. Add regions and rental products only after their own tests. | P1 |

Signing credentials and supplier qualification can proceed while engineers stabilize clients. Keep the control service in a separate application/repository; provider/payment secrets never belong in desktop clients or public images.

P0 entry requires same-SHA platform tests, acceptance of the exact candidate packages (prefer signed/notarized), resolved defects in advertised features, one two-hour real mixed-platform GUI session including TCP fallback, the scripted permission/reconnect/teardown checklist, a 24-hour server soak including slow receivers, visible limits, and a timed restore. P0 is explicitly preview research; final native/endurance gates remain required before stable promotion and the paid managed pilot. This lets observed support/egress inform supply decisions while retaining the full public gates. Critical correctness, data-loss and recovery failures never become acceptable merely because a pilot is small.

## Smallest credible deployment

Use a versioned cloud-init/bootstrap input and the existing native package/service. Prefer Debian 12 initially; do not silently install a Debian artifact on Ubuntu merely because both use `.deb`. A one-click image is a delivery format for a proven installer, not a substitute for it. DigitalOcean supports initial [user data/cloud-init](https://docs.digitalocean.com/products/droplets/how-to/provide-user-data/); other adapters require equivalent validation.

The deployment should validate inputs and OS, install a pinned verified artifact, generate a unique server/TLS identity and admin secret, create persistent directories with narrow ownership, apply explicit room/media limits, and start/enable the service. Validate TCP connection and an actual authenticated UDP exchange externally; a running process or open port is insufficient. Deliver a connection link with the assigned port and a secure one-time credential handoff. Never leave reusable customer secrets in images, user-data logs, ordinary logs, or downloadable support bundles.

The existing protocol relays voice/video/files through the community server. There is no need to invent P2P/STUN/TURN to launch this design. Open the assigned Mumble port for TCP and UDP, validate IPv4/IPv6 and blocked-UDP fallback, and keep administrative Ice/host APIs private with credentials. With shared IPv4, persist a unique TCP+UDP pair on the same port and validate firewall/NAT removal/reassignment without interrupting other tenants.

Generate customer secrets inside the guest, not inside provider user-data. Validate a secure authenticated handoff and remove temporary handoff material; metadata/log retention makes user-data unsuitable for persistent admin secrets.

An idempotent lifecycle must reconcile partial failures: retries cannot create duplicate VMs, ports, charges, or credentials. Start rental time when connectivity is ready. Separate provider deletion from process shutdown; reconcile disks, addresses, backups and orphan resources. Give customer root only inside their guest. Enforce CPU/RAM/I/O/network caps, isolation and anti-spoofing outside the guest, and test that a noisy/root tenant cannot reach management or starve another community's voice. Reassign only after verified clean reimage and new identities; quarantine failed resets.

Back up SQLite using its supported consistent backup mechanism or a stopped-service export, not an arbitrary live file copy. Include config, registered users, channels, ACLs, bans and server TLS identity. Retain portable encrypted state according to the existing seven-day post-expiry proposal; validate customer export and deletion. A VM snapshot alone is not a portable application backup. Before upgrade, preserve binary/config and pre-migration state; never downgrade a database without a tested compatible migration or restore. Proposed recovery targets are RPO 24 hours and RTO 30 minutes for application state, subject to a timed drill and replacement capacity. These are not live failover guarantees.

## Economics and commercial decision

Measure support and fan-out before committing to a low price. At 1.5 Mbps target webcam rate, ten simultaneous senders with nine receivers require 135 Mbps outgoing payload, about 60.75 decimal GB/hour; protocol overhead and voice add more. The historical fixture exceeded its encoder target. File chunks also fan out to capable channel members, so file traffic must be included. The server's aggregate limits protect throughput but currently shed packets; they are not a complete monthly transfer-quota/product mechanism.

DigitalOcean's published reference price on October 10 is $6/month for a 1 GiB VM with 1,000 GiB transfer. Daily percentage-based backups add 30%; excess egress is $0.01/GiB. [VM pricing](https://www.digitalocean.com/pricing/droplets), [backup pricing](https://docs.digitalocean.com/products/backups/details/pricing/), [transfer billing](https://docs.digitalocean.com/platform/billing/bandwidth/). These are comparison rates, not a selected supplier or proof that a 1 GiB guest sustains our advertised workload.

Use the repository's capacity model for shared-host scenarios. Keep historical supplier tariffs separate from new quotes; the existing $104-host example is an assumption, not reconfirmed procurement. For a US domestic card subscription, the current Stripe reference is 2.9% + $0.30 payments plus 0.7% Billing. [Stripe pricing](https://stripe.com/pricing). Other cards/countries/products can cost more.

Illustrative monthly sensitivity, assuming $2 support allowance per community:

| Scenario | Contribution before unentered costs |
| --- | ---: |
| $9.99 subscription on a $6 individual VPS | $1.33/customer after fees and support, before backups/operations |
| Same with 30% daily VM backup surcharge | −$0.47/customer before other operations |
| $19.99 on the same VPS + daily backups | $9.17/customer before other operations |
| Existing $104 shared host + $50 fixed operations, 40 × $9.99 subscriptions | $139.21/node/month after $2/customer support reserve |
| Same shared host with $10/customer support instead of $2 | −$180.79/node/month |

These are calculated contributions, not net profit or validated customer capacity. They exclude tax, acquisition, refunds/disputes, setup amortization, surplus transfer, further state-archive costs, signing costs and labor outside entered allowances. At an assumed $40/hour, a $2 allowance buys only three support minutes per customer-month. Ten minutes costs $6.67. A profitable hardware model can fail quickly through platform support.

For the $154 fixed shared-host example, per-customer contribution is $7.33036 and break-even is 22 paying customers. This remains unusable if safe density is below 22 or observed evening demand exceeds bandwidth. Adding $104 spare capacity moves break-even to at least 36 before spare operations. Do not buy shared supply on the strength of modeled allocation ratios.

Use a commercial pilot gate: positive realized contribution after measured support and every committed expense; proposed target at least 30% contribution margin and median support under ten minutes/customer-month. The margin gate governs the price, irrespective of the support-minute target. Calculate break-even, acquisition payback and cash commitments separately; contribution does not pay all development costs automatically. Require repeat use and willingness to renew at the actual price. Reduce advertised media/concurrency or change price/supply if the workload fails these gates; never assume unlimited video or average occupancy fixes a loss.

Include actual monthly video-hours, not just instantaneous throughput. At the idealized 135 Mbps all-webcam payload, a full 1,000 GiB transfer allowance lasts about 17.7 hours before voice/overhead/files. Sixty hours means about 3,395 GiB of payload; roughly 2,395 GiB above that allowance costs $23.95 at the quoted $0.01/GiB reference rate. That can consume the entire $19.99 revenue before VM/support costs. This calculation is illustrative, not a workload measurement. A 200 Mbps aggregate pilot cap permits approximately 90 decimal GB/hour of video alone; it is not a suitable default for an unmetered low-price offer without supply qualification. Choose tiers with explicit media/concurrency/transfer limits and a tested action at exhaustion. A connection count by itself does not price video fan-out. Compare file routing costs before assuming a one-recipient transfer has one-copy egress.

The standard root-access support remedy should be a documented reimage-and-restore; investigating arbitrary guest modifications must be outside the included standard-setup support promise. Include signing-service commitments in fixed costs and measure the actual support boundary during P0/P1.

## First reviewable milestone

The first milestone is a stable candidate with real three-platform installed-client evidence and a tested one-server restore, before checkout development. Start with S1–S3, while preparing signing and D1/D2. Then complete media-budget behavior and native acceptance. Use H1/H2 to establish the existing bare-metal proposal's actual safe capacity, followed by the private monthly pilot.

Use P0 to measure real usage/support before buying shared-host supply; it does not replace the stable candidate or H1/H2 qualification for managed paid hosting.

Current verdict: suitable for continued engineering and a bounded technical pilot; not supported by sufficient evidence for public paid one-click hosting. The reasons are specific: the outstanding GUI file-transfer defect, missing Windows/macOS test execution, default multi-camera aggregate loss, unqualified installed-client quality, and unvalidated lifecycle/recovery/density economics.

### Immediate action items

- [ ] Assign owners and resolve S1 candidate blockers.
- [ ] Make S2 same-SHA cross-platform tests required for stable promotion.
- [ ] Add S3 production transport coverage and record actual GUI pair results.
- [ ] Prepare signing credentials and versioned D1/D2 deployment/restore prototypes.
- [ ] Complete the [acceptance record](release-acceptance-record.md) before promotion or paid-pilot admission.

## Opus review and adjudication

The completed [Opus 5.5 CLI assessment](opus55-stability-review.md) used the supplied source excerpts, verified findings and roadmap. It was not an independent full-repository audit. An initial maximum-effort exploratory CLI pass was stopped after 20 minutes without returning a report; the successful evidence-based pass used the exact `claude-opus-5-5` model with high reasoning effort and no tools.

Adopted: exact stable-configured signed-artifact acceptance, earlier friendly measurement before bare-metal investment, slow-receiver tests, platform lifecycle verification and explicit monthly-egress sensitivity. The reviewer independently checked the arithmetic from the supplied assumptions.

Retained as investigation tasks: worker access to the shared server handler and reconnect races (consider TSan); legacy video delivery; file-data fan-out and write-buffer growth. Source confirms video currently reaches all other channel members and file chunks all capable members, while file control is targeted. Their interoperability/privacy/economic consequences require a reproduced test and protocol-design review. `screen_sharing` is sender state, not evidence of a video-receive capability; do not blindly filter on it. Similarly, changing fragment-drop behavior is not an established decoder-recovery fix. Keep these distinct from the documented default-budget failure and PR 29 defect.

Not adopted as guarantees: the reviewer's proposed 40 Mbps profile, $19.99 price floor, or an assumption about most users' macOS version. Those require workload/market validation. Its rough decimal-GB transfer-overage example was converted to billed GiB in the calculation above. Debian 12 is not unsupported: verify its LTS/dependency coverage and compare Debian 13 before a production choice.

## Planning validation

- Fresh isolated checkout pulled from `origin/master`; original dirty checkout preserved.
- Eight release-publication unit tests passed locally.
- Standalone Qt `TestReleaseUpdate` configured, built and passed locally on macOS.
- Twenty hosting-tool regression tests passed locally; native bridge tests were not run in this review.
- Baseline hosted CI statuses and the open PR 29 description inspected. PR checks were still in progress at inspection; its fix was not treated as merged or GUI-validated.
- Completed exact-model Opus 5.5 CLI evidence-based review saved alongside the roadmap; hypotheses and adopted changes adjudicated above.
