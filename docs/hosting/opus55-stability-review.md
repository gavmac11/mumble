# Opus 5.5 CLI assessment

Date: October 10, 2026. Baseline: origin/master `7fc94bf23`.

Method: Claude CLI, exact model `claude-opus-5-5`, high reasoning effort, no tools. The reviewer was supplied the proposed roadmap, verified repository findings, primary source excerpts, existing pilot results, release/update documentation and local validation results. It did not independently inspect the full repository or run tests. An earlier unrestricted-in-reading, maximum-effort repository pass was stopped after 20 minutes without returning a report; no conclusion is attributed to that pass.

This is the model's assessment, retained as review evidence. Its hypotheses and proposed prices are not confirmed defects, current vendor quotes, or adopted design decisions. Adjudication is in the roadmap's Opus review section. In particular, changing frame-drop behavior or capability routing requires protocol/lifecycle review, current platform lifecycle must be sourced, and decimal GB must be converted to billed GiB in transfer calculations.

---

# Assessment: ADR-001 stability-to-hosting roadmap

This review uses only the supplied evidence and excerpts. I ran no tests, opened no PRs and checked no new sources. Items marked *verify* are my inferences.

## Verdict

The direction and the current verdict are right. Continue engineering and a bounded technical pilot. Public paid one-click hosting is not justified yet. I recommend four amendments:

1. **Fix PR 29 so this class of bug cannot recur.** Patching the one symptom is not enough.
2. **Close two gaps the ADR misses:**
   - The stable channel requires a different build configuration.
   - Video and file data are sent to channel members who do not need them.
3. **Split the gates into "pilot-entry" and "public".** A small provider-VPS pilot can then measure support minutes and egress before any bare-metal commitment. Those two numbers decide profitability.
4. **Re-check the dated OS targets.** These are Debian 12 and macOS 15.

## 1. Causal claims, adjudicated

| Claim | Assessment |
|---|---|
| **PR 29: the string-based invoke fails** | **Plausible, with two candidate causes.** `FileTransferManager.cpp:51` and `:59` call `invokeMethod` by name and ignore the `bool` result. The `ServerHandler.h:192-194` excerpt does not show the access-specifier section, so "plain method" rests on your verified finding. A second cause is also possible: `Q_ARG(std::optional<quint64>, …)` at `:61` must be a queued-marshallable metatype. Either way the call fails with only a `qWarning`, which is the "silent permanent failure" Gate 4 forbids. |
| **Existing E2E test misses the broken path** | **Confirmed.** `TestFileTransferE2E.cpp:6-13` describes its own transport with no GUI. It never runs `setupEngineTransports()`. |
| **Default 2.5/20 Mbps limits caused the two-webcam loss** | **Strongly supported, not proven.** 20 Mbps out of a 29.51 Mbps replicated payload is about 68%, and 65.5% of deliveries arrived. The October 3 run at 2.5/200 isolates the aggregate limit, but it used a different date and build, and no per-guard counters exist. The claim holds for the current encoder output, since the fixture exceeds the 1.5 Mbps target. It is not universal. |
| **Packet loss maps to frame loss** | **The ADR understates this.** Losing 34.5% of packets left only 292–303 of 900 complete frames. `Server.cpp:1232-1235` sheds individual fragments, so loss amplifies roughly twofold at the frame level. |
| **Preview can publish without full client CI** | **Confirmed.** See `preview-installers.yml:24` (`-Dtests=OFF`), `:621-626` (the `needs` list excludes `build.yml`) and `build.yml:56-64`. The shipped ARM macOS architecture is not built by `build.yml` at all. |
| **Static Qt test packaging is the Windows/macOS blocker** | **Likely.** `src/tests/CMakeLists.txt:18-28` stops with `FATAL_ERROR` when the offscreen plugin is missing. |

**PR 29 fix to require.** Use the functor overload: `QMetaObject::invokeMethod(raw, [raw, …]{ raw->sendFileData(…); }, Qt::QueuedConnection)`.
- Use the `ServerHandler` raw pointer as the context object. If the handler is destroyed, Qt drops the queued call.
- Do not capture the `shared_ptr` in the lambda; that would extend the handler's lifetime.
- The call becomes compile-checked and needs no metatype registration.
- If PR 29 only adds `Q_INVOKABLE`, it works, but the rest of the bug class stays open.

**Related unverified risks:**
- **Thread comment may be wrong.** The comment at `FileTransferManager.cpp:45-46` says sends go to "the ServerHandler thread (which owns the socket)". Queued calls run on the thread the receiver *object* belongs to. A `QThread` subclass normally belongs to the thread that created it, not to its own `run()` thread. This is probably harmless, since the GUI calls the other request methods at `ServerHandler.h:180-203`. S3 should still assert delivery and ordering rather than assume a thread (*verify*).
- **Possible data race.** `Global::get().sh` is copied on the worker thread at `:49` and `:57`. That non-atomic `shared_ptr` read can race with disconnect. Add reconnect-during-transfer to the ASan/TSan nightly.

## 2. Gaps the ADR misses

**a. The stable channel is a separate build.** `docs/dev/ClientUpdates.md` says stable-only clients need `-Dupdate-prereleases=OFF`. That leaves two bad choices:
- Promote the tested preview binary to stable. Stable users then keep receiving previews.
- Rebuild for stable. The shipped binary then differs from the tested one, and signing changes the bytes again.

Gate 2 must therefore run on the **exact signed, stable-configured artifacts**. Do not add a runtime channel switch now; it is feature creep.

**b. Video is relayed to every channel member.** `Server.cpp:1268-1275` checks no receiver capability. By contrast, file data checks `bFileTransferCapable` (`Messages.cpp:2682,2707`).
- Legacy and upstream clients in a mixed room receive video UDP they cannot use.
- The aggregate charge at `:1230-1231` counts them, so they consume budget.

This directly affects your mixed-capability interop gate and egress cost. If a video capability flag exists, skipping non-capable receivers is a small server change. If upstream clients misbehave on receipt, it becomes a blocker.

**c. File data is broadcast while control is targeted.** `msgFileTransferControl` routes only to named targets (`Messages.cpp:2617-2626`). `msgFileData` sends every chunk to all capable channel members (`:2705-2711`).
- If transfers are pairwise, a 1 GiB send in a ten-user channel costs about 9 GiB of egress.
- Every capable client also downloads ciphertext it will discard.

Confirm the design intent. If transfers are pairwise, routing `FileData` to recipients only is the highest-value economic fix visible in this code.

**d. 1 MiB chunks over TCP** (`Messages.cpp:2646`) carry two risks:
- TCP-fallback users have voice tunnelled on the same stream, so chunks can block it (head-of-line).
- Per-receiver server write buffers, multiplied by fan-out to a slow receiver, may grow without bound.

Add a throttled-receiver case to Gate 5.

**e. Platform targets have aged.**
- Debian 12 has likely left regular security support and moved to LTS by October 2026 (*verify*). Settle the server OS before automating D1, since Debian 13 may be the better baseline.
- macOS 15 as the minimum is fine, but most Macs will run 26 or newer. Test the current and previous major releases.

**f. Cloud-init secrets.** Keep the admin secret out of user-data entirely, because the metadata service retains it. Generate the secret on the guest and hand it over through an authenticated channel.

## 3. Release blockers vs unverified risks

| Item | Classification |
|---|---|
| PR 29 file-transfer send path | **Blocker if file sharing is advertised.** The alternative is to ship the candidate with file sharing off; commit `0c1914038` suggests the disabled build compiles. |
| No tests on shipped Windows / ARM macOS; preview publish not gated on them | **Blocker** for stable promotion |
| Stable build configuration vs tested artifact | **Blocker** for stable promotion |
| Default profile fails two webcams in ten users | **Blocker** for any advertised multi-camera claim. Choose and disclose a profile. |
| Untested backup/restore | **Blocker** for paid use |
| Unsigned Windows / unnotarized macOS | **Blocker for public release.** Tolerable for a friendly pilot, though macOS 15+ makes unnotarized launch painful. Notarize before the pilot if possible. |
| Video to non-capable receivers; file broadcast | **Unverified risk.** Becomes a blocker on interop failure or a confirmed pairwise design. |
| `sh` cross-thread copy; TCP write-buffer growth; fragment-level shedding | **Unverified risks.** Cover in nightly sanitizers and Gate 5. |
| Bare-metal density and economics | **Unverified.** Gates the P1 supply commitment, not client work. |

## 4. Smallest valuable sequence

1. **S1.**
   - Merge the PR 29 fix in functor form, check the result and surface failure in the UI.
   - Add one test at the manager-to-`ServerHandler` boundary.
   - Decide explicitly whether the candidate ships file sharing.
2. **S2.**
   - Run tests on Windows x64 and ARM macOS, and make publication depend on them.
   - If the static offscreen plugin takes more than a few days, add an interim shared-Qt test job on those OSes. It catches logic regressions but not packaging, so state that limitation in the gate.
3. **S4-lite (server only, no protocol change):**
   - Add per-guard drop counters.
   - Send the sender one rate-limited existing permission-denied or text notice on first refusal.
   - Once a fragment is refused, drop the rest of that frame.
   - Skip receivers that cannot use video.
   - Ship one named profile. For example, qualify roughly 40 Mbps aggregate for "two webcams in ten users": 29.51 Mbps measured plus overhead is my estimate and must be tested.
   - Keep budget negotiation deferred, as documented.
4. **Stable build path.** Configure stable builds and sign/notarize them. Acquire credentials in parallel now.
5. **D1/D2 on one provider VPS.** Pinned artifact, external TCP and UDP checks, and a timed restore drill.
6. **P0, a new step.** Run 3–5 friendly communities, free or discounted and clearly labelled preview, on provider VPSs for 30 days. Measure:
   - support minutes per community
   - video-hours and egress
   - restore events

   This data comes before H1/H2 investment, because it is the input your capacity model lacks.
7. **Then:** full native acceptance (S5), H1/H2 on the actual supply, the P1 paid pilot and P2.

## 5. Tiered gates

**Pilot-entry (P0 and P1):**
- Gate 1 in full.
- Gate 2 on signed artifacts for the three named targets.
- Gate 3 reduced to one two-hour, ten-person mixed-media session, including forced TCP.
- Gate 4 as a scripted manual checklist.
- Gate 5 as 24 hours of automated server load, including a throttled file receiver.
- Gate 6 with counters and the chosen profile.
- The D2 restore drill, timed.

**Public (P2):** every ADR gate as written, plus H2 density and the commercial gate.

This keeps the ADR's rigor and avoids staging ten real participants on three platforms for 48 hours before anyone has paid.

## 6. Economics

The ADR's arithmetic checks out: $1.33, −$0.47, $9.17, $139.21, −$180.79, $7.33036 per customer, and break-even at 22 and 36. Three points need more weight.

**Egress is the hidden variable, and the aggregate cap is the pricing lever.**
- The all-webcam scenario uses 60.75 GB/h. That is roughly 17 hours of $6-VPS transfer allowance per month.
- At 60 such hours a month, about 2.6 TB of overage costs roughly $26 at $0.01/GiB. That alone sinks both the $9.99 and $19.99 VPS tiers.
- The configured cap bounds worst-case egress: 20 Mbps is at most 9 GB/h, while 200 Mbps (`pilot-server.ini`) is 90 GB/h.
- On a shared host, 40 communities on the raised profile is 8 Gbps worst case. The port capacity is an assumption to verify with the supplier.

Price tiers by aggregate media cap, not by user count. Measure concurrent video-hours in P0 before setting density.

**Support dominates, and root access drives support.** Your $2 allowance buys three minutes. The support boundary must state that the remedy is reimage-and-restore, never debugging a customer's root changes. Otherwise the $10-support row (−$180.79 per node) is the realistic one.

**Price floor.** $9.99 on an individual VPS is not viable once backups exist. Use $19.99 as the pilot price. Revisit $9.99 only on shared supply, with measured density of at least 22, support under about $2, and file fan-out fixed. Add the fixed signing costs (an Apple Developer Program membership plus a Windows signing service) to the fixed-cost line.

## 7. Trade-offs and what to avoid

- **Shipping without file sharing:** a faster candidate, but you lose a differentiator. This is the right choice if PR 29 cannot be made compile-checked and covered quickly.
- **Interim shared-Qt tests:** faster coverage now, at the cost of a known gap on the static packaged binary.
- **Early P0:** real data at some reputation risk. Contain it by limiting P0 to friendly communities with explicit preview terms.
- **Server-side refusal notices instead of negotiation:** less user experience, no protocol churn, and enough for a bounded profile.
- **Do not build yet:** budget negotiation, unattended updater, runtime channel switching, hourly or annual rentals, P2P/TURN, more platforms or codecs.

## Implementation priorities

1. PR 29 functor fix with boundary test, or a decision to ship with file sharing off.
2. Same-SHA Windows and ARM macOS tests gating publication.
3. Server-side media counters, frame-level drop, receiver-capability filtering and a named profile.
4. Stable-configured signed artifacts as the only object of Gate 2.
5. Debian target decision, then D1/D2 with a timed restore.
6. P0 measurement, before H1/H2 and any supply purchase.
