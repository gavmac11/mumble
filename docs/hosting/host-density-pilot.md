# Host density pilot

Status: the [initial application pilot](pilot-results-2026-10-01.md) exposed video bursts that also dropped voice. The [October 2 pacing tests](pilot-pacing-results-2026-10-02.md) and [October 3 per-sender comparison](pr25-review-followup-2026-10-03.md) passed short media workloads with a raised **200 Mbps aggregate video limit**, not the server's default 20 Mbps aggregate limit. Client pacing does not prevent aggregate-limit drops or report them to the sender. GUI quality, sustained workloads, KVM density and the full workload matrix remain untested. This plan is intended to validate the [bare metal launch proposal](launch-research.md); thresholds below are proposed gates, not customer guarantees.

## Inputs to record

Record the provider, region, exact SKU and quote, physical cores/threads, RAM, disks and RAID, NIC/link guarantee, transfer policy, host OS, Proxmox version, guest image checksum, application commit, client versions, and every resource limit. Record host and guest clock synchronization, client locations, and load-generator capacity. Do not compare results across changed configurations without recording the change.

Use external client machines with enough aggregate network, encoder, and decoder capacity. Ten webcams require roughly 15 Mbps of video into each community server and 135 Mbps out. A generator that cannot receive/decode its assigned load invalidates the test. Monitor generators separately. Clients on the target hypervisor would bypass or compete with resources in ways that distort the advertised workload.

The repository's protocol and AudioReceiverBuffer benchmarks are microbenchmarks. The preliminary [relay probe](../../scripts/hosting/relay_probe.py) authenticates protocol clients and checks synthetic or encoded video fan-out over TCP/TLS or encrypted UDP, optionally with encoded Opus audio. H.264 captures are decoded separately. It does not run the GUI, listen to audio, measure playout, or create multiple communities. Generic traffic alone cannot establish voice/video quality.

The supplied test machine presents 8 vCPUs and about 12 GiB RAM inside KVM and runs existing services. Use it for a bounded preliminary application pilot with an isolated checkout and explicit CPU/RAM limits. Its virtual CPU topology and nested execution do not establish bare-metal density. Do not replace its host OS with Proxmox or treat all RAM as available for this test.

## Resource collection

The standard-library [Linux collector](../../scripts/hosting/collect_metrics.py) reads CPU, memory, pressure, and interface counters without changing host settings. Run it on both the Linux server host and each guest, with separate output files:

    python3 scripts/hosting/collect_metrics.py --samples 600 --interval 1 --interface eth0 --output host-run-001.jsonl

Use the real egress interface name; omit the interface option to report all interfaces separately. Do not add physical-NIC and bridge/tap counters together. Output creation is exclusive, so an existing result is not overwritten. Missing/reset counters produce null intervals; unavailable pressure sources remain explicit.

This collector does not provide disk latency, thin-pool metadata, per-VM attribution on a host, NAT tracking, Mumble relay counters, or decoded media quality. Those observations remain required from hypervisor/application instrumentation and clients. Kernel counter definitions: [proc](https://docs.kernel.org/filesystems/proc.html) and [pressure stall information](https://docs.kernel.org/accounting/psi.html).

Run local accounting checks with:

    python -m unittest discover -s scripts/hosting -p "test_*.py" -v

## Preliminary application build and relay probe

Use an isolated checkout of the pinned commit with all submodules initialized, including license-only dependencies. The [build script](../../scripts/hosting/build_pilot.sh) requires an explicitly designated disposable Debian container and builds a SQLite-only server with two compiler jobs. Cap the container to two CPUs and 3 GiB RAM. This pilot disables LTO and downgrades only the known GCC 12 restrict diagnostic from error to visible warning; it does not change application source. It is not the final production build profile.

Choose one of the two named configurations without modifying its bandwidth fields:

| Configuration | Per-sender video cap | Aggregate relayed-video cap | Purpose |
| --- | ---: | ---: | --- |
| [pilot-server-defaults.ini](../../scripts/hosting/pilot-server-defaults.ini) | 2.5 Mbps | 20 Mbps | Qualify both default video limits, including multi-sender failures. |
| [pilot-server.ini](../../scripts/hosting/pilot-server.ini) | 3.5 Mbps | 200 Mbps | Raised-limit hosting workload and historical October 1–2 reproduction. |

Copy the selected file to the mounted runtime directory as server.ini before starting the server, and save that effective file with the results. The October 3 result is an archived **per-sender-only comparison** at 2.5/200 Mbps; use its [recorded effective configuration](results/2026-10-03/pr25-final-default-sender-server.ini) only to reproduce that historical comparison. It is not the defaults recipe, and counts from different aggregates must not be presented as the same configuration.

Keep port publication private: loopback for a TCP SSH tunnel, or an authenticated private network address for direct TCP/UDP. Use a test certificate valid for the probe hostname. Record tunnels and overlays as part of the measured path; an SSH TCP forward does not test UDP.

Example after the private server and tunnel are running:

    python scripts/hosting/relay_probe.py --ca-file /path/to/pilot.crt
    python scripts/hosting/relay_probe.py --ca-file /path/to/pilot.crt --senders 10 --sender-mbps 1.5 --seconds 10

The probe verifies TLS certificates, payload integrity, unique fan-out counts, control-ping RTT, and generator event-loop lag. Its delay measurement includes client scheduling and the entire network/relay path. Undelivered packets at the deadline are not a direct measurement of network packet loss. Compare the default aggregate cap with the proposed 200 Mbps setting, recording the restart and configuration for each run.

For UDP, compile the [native bridge](../../scripts/hosting/build_crypto_bridge.sh) in the same bounded build container. It uses unchanged repository crypto sources and installed Qt/OpenSSL dependencies. Set MUMBLE_PILOT_CRYPTO_LIBRARY to the resulting library when running [native checks](../../scripts/hosting/test_native_crypto.py). Run the generator in its own limited environment, with only the public test certificate mounted; never mount or copy the server private key into the generator.

Example using the generated [fixtures](pilot-results-2026-10-01.md#encrypted-udp-and-concurrent-voice), after preparing a private server endpoint:

    python3 scripts/hosting/relay_probe.py --host PRIVATE_ADDRESS --port PRIVATE_PORT --server-name localhost --ca-file /pilot.crt --transport udp --crypto-library /build/libmumble_pilot_ocb.so --clients 10 --senders 1 --seconds 5 --fps 15 --width 1920 --height 1080 --h264-fixture screen.h264 --voice-senders 10 --opus-fixture voice-noise.opus --capture-directory screen-capture

Use ten webcam senders at 30 fps and 1280×720 for the burst case. Compare synchronized starts with --stagger-video-frames, preserving both results. Record /proc/net/snmp UDP counter changes inside the server namespace and on the client host, plus both cgroups' CPU counters. A generator CPU cap, socket queue, or application limiter can constrain the test before physical host capacity does. Do not erase failed runs or interpret a short passing case as a production limit.

For the current client pacing comparison, compile the [pacer bridge](../../scripts/hosting/build_pacer_bridge.sh) and add --native-pacer-library plus --video-send-cap-mbps 1.972668 for webcams or 2.4 for screen sharing, matching VideoQuality::wireBitRate with the current profiles. The probe uses the actual C++ PacketQueue with Python scheduling; recorded fixtures cannot respond to encoder keyframe requests. The historical October 2 rates of 2 and 3 Mbps apply only to the earlier patch and its archived evidence.

Set MUMBLE_PILOT_PACER_LIBRARY to the compiled bridge when running the Python tests to include the 2,049–4,096-packet large-frame boundary checks. The probe rounds Mbps to whole bps; the older October 3 probe truncated the webcam allowance to 1,972,667 bps, one below the client value. Its raw records are retained unchanged.

Keep scheduled-frame-to-delivery delay alongside relay delay so pacing latency remains visible. Probe schema 2 records offered, accepted and actually submitted fragments separately. expected_deliveries and undelivered_by_deadline now describe submitted traffic; pacer_rejected_fragments and accepted_not_submitted_by_deadline expose client-side losses or remaining queued work. total_missing_deliveries includes both client and relay deficits, and complete_fanout still fails if either loses media. Captured-frame completeness remains an independent check. Actual GUI sending and playout need separate tests.

For the fully default video-limit check, use pilot-server-defaults.ini with ten clients and two webcam senders. Nine-way replication puts even two 1.5 Mbps encoded streams above the 20 Mbps aggregate allowance before overhead. The server may silently discard video despite every sender staying below 2.5 Mbps. This is a workload limitation to record, not a failure to hide by raising the cap in the defaults recipe.

The [October 4 comparison](pilot-default-limits-2026-10-04.md) recorded this failure: 85,680 of 130,896 video deliveries arrived at 2.5/20 Mbps, versus all 130,896 at 3.5/200 Mbps. Both runs delivered all voice and had no client pacing drops. The templates and effective limits are recorded beside each result.

## Budget negotiation follow-up

Track a protocol follow-up to advertise the server's actual per-sender video limit and server-wide aggregate egress limit during connection setup, including disabled limits and changes made while connected. The client must clamp encoder and pacer budgets together to the negotiated allowance. Aggregate capacity also needs an explicit allocation or admission policy as senders, recipients and channels change; merely advertising a per-sender cap cannot prevent aggregate drops.

Acceptance checks should cover videobandwidth=1500000, the 2.5/20 Mbps defaults, raised limits, legacy peers without advertisement, configuration changes and multiple senders/channels. Define visible feedback when a requested quality cannot fit and when the server drops media for a budget limit. Until this is implemented, the client uses its default-budget fallback without detecting lower custom caps or aggregate exhaustion. The current patch does not resolve that negotiation gap.

## Guest and network preparation

1. Build the fork's pinned Debian server package with its existing packaging workflow. Verify the artifact checksum and matching client build.
2. Create a clean Debian guest template with cloud-init or equivalent first-boot configuration. Generate unique SSH identity, credentials, and initial application identity for each new community.
3. Start with 1 vCPU, 1 GiB fixed RAM, and a 15 GiB disk. Apply CPU, memory, storage, I/O, and NIC limits on the host. Measure hypervisor overhead before admitting more guests.
4. Set the managed pilot to ten users. For default video-limit qualification, copy pilot-server-defaults.ini unchanged (2.5/20 Mbps). For the raised-limit hosting workload, copy pilot-server.ini unchanged (3.5/200 Mbps). Record both effective limits and the template used beside every result. The default 20 Mbps aggregate cap cannot carry ten full webcam streams regardless of pacing; raised-limit results do not qualify the default configuration.
5. Give the guest a private IP, one public Mumble port forwarded for TCP and UDP, and a separate SSH port. Verify invite links, normal UDP, TCP fallback, guest root access, and isolation from tenants and management.
6. Install collection for host/guest CPU, run queues, steal time, memory pressure, swap, disk latency, thin-pool usage, NIC bytes/packets/drops, NAT tracking, and Mumble/client quality. Confirm the timestamp and units of each counter.

The initial proposed virtual-NIC cap is approximately 200 Mbps, subject to measurement. Keep the provider's real egress and packet-rate limits in the test configuration. An application setting is not a host-side quota when a customer has root.

## Workload sequence

| Stage | Workload | Evidence |
| --- | --- | --- |
| Single community | Five, then ten clients; voice-only; one screen; all webcams; one screen plus remaining webcams. Include normal speech bursts and synchronized stream/keyframe starts. | Two hours of mixed media, decoded video and recorded synthetic test audio, actual bitrates, latency/loss, memory and CPU. |
| Idle guest overhead | Boot 8, 16, and 24 guests; then 32 and 40 only if RAM/storage budgets permit. No ballooning or swapping to fit guests. | Per-guest idle footprint, host reserve, disk use, startup time and boot-storm effects. |
| Active density | At each candidate density, run defined counts of screen-only, all-webcam, mixed-media, and voice-only communities; leave the rest connected or idle as specified. | Repeatable results for the exact workload mix, not only a total VM count. |
| Correlated demand | Start many communities within 60 seconds. Test assumed 25%, 50%, and 100% active fractions and event-like webcam bursts. | Where queueing/loss begins; whether voice remains usable. These fractions are scenarios until real pilot demand is observed. |
| Resource contention | On test-owned guests, apply CPU, disk-fill/I/O, and network pressure up to their advertised limits while ordinary communities run. | Host enforcement, tenant fairness, voice stability, storage safety, and management access. |
| Lifecycle contention | Export state, provision/reinstall guests, expire a rental, and boot guests while ordinary calls continue. | Provisioning and expiry correctness without damaging ongoing calls. |
| Endurance | Repeat the selected mix for at least 24 hours, then observe a private pilot for at least seven days including an evening/weekend peak. | Leaks, drift, correlated real demand, support effort and a repeatable admission decision. |

For each density, include the calculated 2 all-webcam + 6 screen-share mix and, when enough guests exist, the 4 all-webcam + 36 screen-share overload case. The latter exceeds a 1 Gbps budget by design. Use it to locate failure and containment behavior; it is not an expected passing workload.

Never describe a VM count alone as capacity. A result is a count, an active workload envelope, a host configuration, and observed quality. If realistic correlated demand cannot fit, reduce admissions, change the permitted media envelope explicitly, or buy more network capacity.

## Measurement and provisional gates

Sample host/guest resources at one-second intervals and retain raw observations. Report median, p95, p99, and worst one-minute intervals; an average can hide voice-damaging spikes. Separate provider-path latency from additional latency introduced by host load.

| Area | Proposed passing evidence |
| --- | --- |
| Voice | No sustained audible dropout in the synthetic test recording; load adds no more than 20 ms to p95 round-trip media latency relative to the same path at baseline; packet loss below 0.5% over each five-minute window. RTT alone does not establish mouth-to-ear latency; measure playout separately. |
| Video | Actual decoded frame rate stays within 10% of the single-community baseline for identical media; no persistent corruption or repeated freezes longer than one second. Record encoder output so lower offered load is visible. |
| CPU and memory | Host CPU p95 below 70%, no persistent run-queue pressure or guest steal above 5%, no OOM/restarts, and no sustained swap. Track the busiest core as well as aggregate CPU. Validate RAM reserve against measured overhead. |
| Network | Egress stays below the planned 80% budget in the admitted workload; no persistent NIC/queue drops; voice passes during video bursts. Record both bits/second and packets/second. |
| Storage | Every sold quota fits the safe pool budget; thin-pool data and metadata retain headroom; exports and a busy guest do not cause voice/video failures. Determine numeric I/O limits from the measured latency curve. |
| Automation | Five fresh assignments reach a usable voice connection within a proposed ten-minute target. Retries create one VM per lease; expiry removes guest, mappings, and chargeable external attachments. |
| Recovery | Fresh-host restore recovers users, channels, permissions, bans, and application TLS identity. Record actual recovery time and the age of the recovered state. Verify export download and seven-day retention/deletion. |

Thresholds may need refinement after the single-community baseline, with reasons recorded before judging density. Do not relax quality gates merely to achieve a desired oversell ratio. An overloaded workload can be useful evidence without being a passing result.

## Recovery and reuse

Create known channels, users, permissions, and bans. Export a consistent encrypted state snapshot off-host. Restore onto a clean VM with a fresh SSH identity and verify login, permissions, and the intended application TLS identity. Test self-hosted restore with the same pinned application.

Destroy and recreate an expired test guest, remove its NAT mappings, and assign fresh credentials. Confirm the new tenant cannot read old blocks or export material. A database reset alone does not undo customer root modifications. Quarantine failed resets rather than returning them to inventory.

Simulate loss of the pilot host only after a validated export exists and the test is isolated. Measure replacement placement, restore, new connection details, and successful voice reconnection. This establishes rebuild recovery, not uninterrupted failover.

## Recording and decision

For every run, save configuration, UTC start/end, sold guests, active communities by workload, connected people, repetitions, raw metrics, quality observations, failures, and the reason for the admission decision. Keep synthetic media identifiable as test data. Record results that fail as well as those that pass.

Use the [capacity calculator](../../scripts/hosting/capacity_model.py) to check network and cost assumptions:

    python scripts/hosting/capacity_model.py
    python scripts/hosting/capacity_model.py --customers 40 --screen-groups 36 --webcam-groups 4
    python scripts/hosting/capacity_model.py --customers 40 --spare-host-monthly 104

The calculator does not run a load test or predict CPU quality. After repeated passing tests, select an admission count below the tested boundary and reserve capacity for overlapping rentals, maintenance, and recovery. Recalculate contribution using the exact quote, measured support time, measured peak demand, setup amortization, and all idle capacity.

Public launch requires both repeatable technical evidence and positive economics. Repeat relevant qualification in each provider/region sold; a passing US host does not certify an EU cloud plan.
