# Integrated review follow-up work order

The exact Opus 5.5 CLI review is retained in [opus-review.md](opus-review.md). Findings require reproduction and qualification; a green baseline suite is insufficient to promote this candidate. This list supplements the existing S1–S5/D1–D2/P0/H1–H2/P1–P2 release order.

| Priority | Work | Acceptance evidence |
| --- | --- | --- |
| First | Private random receive storage | Owner-only directory/plaintext/password spool; pre-created file and Unix symlink cannot redirect writes; saved output survives while abort/failure/shutdown remove only the owned staging directory. Run native Mac, Windows and Linux. |
| First | Cross-transfer chunk routing | Buffer transfer IDs with early chunks; drop mismatches without extending unrelated job timeouts. Actual server: one sender sends different files to two recipients concurrently; both hashes match, nonrecipients do not receive file chunks. |
| First | Delivery and budget refusal | Align negotiated sender rate with server limits; report refusal and recipient completion accurately. Files exceeding the 360-slot meter window and concurrent recipients must either finish byte-for-byte or show a specific failure, without false Saved state. |
| First | Sender/receiver backpressure | Bound worker-to-GUI, plaintext and TLS queues; control/voice fairness under limited upload. A slow but healthy receiver must retain its session. Measure process memory and delivery at several limited rates. |
| Next | Trust cache and presented identity | Refresh on joins/channel changes; verification binds the key actually shown. Previously pinned changed key stays blocked and never acquires verified trust for the old cached key. |
| Next | Auto-save safety | Separate manual-save history from auto-save folder. Refuse hidden/startup, control-character, alternate-stream and Windows device names for automatic saves. |
| Next | Fatal startup errors | Private negative controls and production-handler child regressions; visible plain-text database dialog, unchanged fallback hashes and flushed fatal diagnostics without unsafe static teardown. PR41 implements this; native/hosted qualification pending. |
| Next | Confirmed replacement | Atomic replacement only after explicit user confirmation; no deletion of existing target before a successful replacement. Error preserves both old target and retryable received file. |
| Next | Recovery readiness and SSH | Pinned TLS/protocol readiness before Ready state, including crash-loop negative control; configured non-default SSH port survives historical restore firewall setup. |
| Next | Packaging and security scan | Packaging fails when required Cocoa repair is omitted; audit dependency reproducibility. Resolve exact-source CodeQL annotations for historical TLS minimum and legacy protocol certificate fingerprint without silently changing the ban identifier. |

After these fixes: freeze an integrated source/package; repeat native/platform matrix, supported installed-client and permission/media checks, signed/notarized packaging, mixed sessions and endurance, and fresh deployment/restore/reboot/interruption drills. PR29 human review remains required. Only then recruit the consenting community preview and measure support time, traffic/egress, provider constraints and multi-guest density before paid price/margin decisions. No current result establishes stable release readiness or profitable hosting.
