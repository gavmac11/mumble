# Stability-to-launch work order

Execution began on 2026-10-10. The [roadmap](stability-to-launch-roadmap.md) remains the acceptance contract. This record tracks completed evidence separately from work that still needs execution. No stable release, public service, supply commitment or commercial pilot is approved by the local test results below.

## Current foundation

Branch: `codex/stability-foundation`, based on `origin/master` at `7fc94bf23cfb7e773282fdd67f30982e5bd25b33`. Initial implementation/CI changes were tested at `4e6c0bee814577c8ef91bff14bcf8309e18deaaa`. Updated lifecycle/IPC sources were tested at `f25f046d41abc34ddeaab7cd4cdc8d3340ccb257`; subsequent evidence-only commits do not change those sources. The separate original checkout and its unfinished capture/audio changes were preserved.

The branch integrates the open [PR 29 candidate](https://github.com/gavmac11/mumble/pull/29) for validation. It has not been merged on GitHub, and its required human approval remains outstanding. [Draft foundation PR 30](https://github.com/gavmac11/mumble/pull/30) must receive updated cross-platform results and review before integration. This is not the frozen release candidate.

Implemented:

- Windows x64, Ubuntu x64, Apple Silicon macOS and Intel macOS regression jobs; tests no longer disabled on Windows/macOS.
- Package-job regression gates on Windows, ARM macOS, Ubuntu and Debian, using the same build that stages the package. Missing/empty suites fail. JUnit and CTest logs are retained on failure.
- Ubuntu builds a server for real relay regression tests, but stages client/default metadata components and the client manual into its client package. A guard rejects accidental server payloads; clean installation remains a CI check.
- Preserved the database-test options when appending the macOS architecture option.
- A typed production file-transfer transport boundary with buffer copies, correct integer/optional values, receiver-thread execution and destruction cancellation.
- Connection snapshots replace worker reads of mutable `Global::sh`. Queued calls do not retain obsolete connections. Identity updates run on the engine worker, with engine setup before starting that worker.
- Actual server disconnects now invoke transfer cleanup, aborting jobs and clearing transport/trust-prompt state before reconnect setup.
- Connection generations reject stale worker trust/password/blocked/progress events after disconnect. Modal trust/recipient/card actions verify their original connection; recipient selection also checks the original channel and user objects.
- Cancelled/declined handshake timers stop and retire. Even a previously dispatched timeout cannot erase a replacement handshake for a reused session number.
- IPC tests receive an explicit platform definition shared by the compiler and Qt metaobject generator, fixing the newly exposed Windows build error; a Windows native-pipe check replaces an otherwise empty Windows target.
- Real-server test fixtures remain in the foreground so their lifetime and startup errors belong to the test process; restored the missing elapsed-timer include.

## Verified local evidence

The initial full client/server build and **34 enabled CTest executables passed** on Apple Silicon macOS 26.6.2, Qt 6.11.1, Apple Clang 21, in 51.48 seconds. The updated lifecycle/IPC build passes **35 enabled executables in 47.49 seconds**, including the actual manager/worker regression target. `OverlayTest` was already disabled and was not executed. SQLite database tests ran; MySQL/PostgreSQL were disabled locally.

The real TLS/server relay suite passed plain/password file transfers and capability gating. These are protocol clients running the identity/trust/engine stack, not interactive GUI acceptance. The standalone production transport tests also passed address/undefined-behavior sanitizers. A deliberately mutated copy using the original string-based Qt dispatch failed as expected, confirming the new transport test detects that regression. Both absent and empty suites were rejected by the CI entry point. Workflow validation, shell validation and whitespace checks passed.

See the [validation manifest](results/2026-10-10/foundation-validation.json) and [CTest summary](results/2026-10-10/foundation-ctest.txt). The local build uses dynamic Homebrew dependencies and external spdlog to avoid a bundled/system-header conflict. Its dependencies target macOS 26 even though CMake targets 15; it provides no evidence of macOS 15 compatibility or the static CI dependency bundles. Installed package, signing, GUI and cross-platform evidence remains unrun.

The [lifecycle manifest](results/2026-10-10/lifecycle-validation.json) and [updated CTest summary](results/2026-10-10/lifecycle-ctest.txt) record the new regressions. Before fixes, queued worker prompts survived disconnect and both cancelled-timer cases kept active timers. Final manager tests feed valid M1 records through the real manager/engine and check GUI-thread delivery and reused-session behavior; interactive dialogs remain untested.

Remote CI for the earlier `63dd192a78ae721c21299b28f394b66bfaa7c30e` foundation passed all 34 enabled tests on Ubuntu shared, Intel macOS static and ARM macOS static. The preview jobs also passed ARM macOS packaging/launch, Ubuntu client packaging/clean installation/launch, and Debian server tests/package installation. Both Windows jobs failed at the same IPC metaobject mismatch; no preview was published. That failure has a local generator check and source fix now, but Windows execution and all updated-head results remain pending. These prior-head results do not qualify the new lifecycle commits.

## Remaining work and gates

| Order | State | Next concrete evidence |
| --- | --- | --- |
| S1 candidate triage | In progress | Required human review of PR 29/foundation; actual declined-identity retry and reconnect GUI checks; review preserved local capture changes; freeze an integrated candidate SHA. |
| S2 regression CI | Implemented; remote results pending | All native/static jobs and clean package checks pass on the foundation; preserve candidate symbols; make exact-candidate checks part of stable promotion. Preview gating does not create stable-channel approval. |
| S3 integration/lifecycle | Partly implemented | Actual GUI identity/recipient/card flows, capability replay, channel changes and media teardown; bounded worker shutdown under heavy transfer preparation; six directed platform pairs. Stale event/timer regressions are automated, but native dialog interaction remains unrun. |
| S4 media budget/diagnostics | Not started in this branch | Measured bounded profile, visible refusal/counters, slow TCP receivers, file fan-out and mixed legacy clients. |
| S5 native packaging/quality | Unrun | Signed Windows and notarized macOS candidates; clean supported OS/session acceptance; two-hour mixed GUI session, 100 cycles and 48-hour endurance. |
| D1 deployment | Next parallel workstream | Pinned, verified, idempotent deployment on a fresh supported guest; external readiness and reboot recovery. |
| D2 state recovery | Unrun | Consistent encrypted DB/config/TLS-state export; restore accounts/ACLs/bans/identity into a fresh guest; timed rollback across the supported schema boundary. |
| P0 friendly preview | Not recruited | 3–5 consenting communities after preview entry gates; measured support minutes, video/file traffic and egress before supply investment. |
| H1 guest lifecycle | Unrun | Persistent shared-IP TCP/UDP/SSH allocation, external VM limits, root-tenant reimage/credential reset and billing reconciliation. |
| H2 supply/density | Unrun | Written current terms and complete costs; physical-host multi-VM correlated load, noisy-neighbor isolation and headroom. Existing single-VM relay measurements are insufficient. |
| P1 paid pilot / P2 launch | Unrun | Qualified gates first; 30-day paid cohort and measured contribution/support/refunds before public one-click launch. |

The [release acceptance record](release-acceptance-record.md) remains unfilled. Do not convert automated build/test passes into native GUI or commercial acceptance.
