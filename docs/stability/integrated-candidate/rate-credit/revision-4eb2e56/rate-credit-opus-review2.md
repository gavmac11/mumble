I found two defects introduced by this patch. Both make the pacer send slower than the configured rate; I found no way for it to burst above its cap. Timer handling, job lifetimes and cancellation look correct. I only read the five supplied files and ran nothing.

## Defects introduced by this patch

**1. Sub-millisecond time is lost, so high configured rates are under-delivered** (`FileTransferEngine.cpp:914`)

- `m_sendPaceClock.restart()` returns whole milliseconds and resets the clock, so any fraction of a millisecond is thrown away on every call.
- The patch now calls `refillSendCredit()` twice per chunk (`:947` and `:1004`). Reading and encrypting one chunk usually takes well under 1 ms, so each of those calls adds zero credit. The time spent sending a batch is never credited.
- Only the idle gaps earn credit: the 50 ms wait at `:968`. The real rate works out to roughly `rate × 50 / (50 + batch work time)`.
- This barely matters at the 4 MiB/s default. Near the manager's 1 GiB/s maximum it can cut throughput substantially. The 0 ms re-dispatch path (`:968`) also loses whatever under-1 ms gap there is between dispatches.
- Fix: keep the clock running and track the last accounted `nsecsElapsed()`, carrying the remainder forward instead of truncating it.

**2. Every `setConfig` throws away banked credit, which can stall slow transfers** (`:96`)

- `setConfig` sets credit to zero unconditionally, even when nothing changed.
- The manager calls it through `refreshPinCache()` → `applyEngineConfig()` on every `startSend` (`FileTransferManager.cpp:221`) and on every pin or verification event (`:81`, `:303`).
- Since the patch requires a whole chunk of credit before sending, a slow rate with a large chunk takes a long time to build up. For example, a 1 MiB chunk at 64 KiB/s takes about 16 s. Any send or pin event inside that window starts the count again from zero, and repeated events can stall the transfer until the receiver's 60 s idle timeout fails it.
- Fix: only reset or clamp credit when the rate actually changes.

## Lesser issues

- **Chunk size vs. receiver timeout** (`:917–926`): settings allow a 1 KiB/s rate with 1 MiB chunks. Waiting for a full chunk of credit then takes far longer than the receiver's hard-coded 60 s idle timeout, so the transfer always fails once the chunk is larger than about 60 s of credit. Nothing validates this, and the whole-chunk rule added here makes the failure certain.
- **Fairness is per chunk, not per byte** (`:956–961`): round-robin gives each job one chunk per turn. A job started after the chunk-size setting changed gets proportionally more bandwidth. When credit runs short, the job at the front also blocks smaller jobs behind it (`:949–951`).
- **Fixed 50 ms wait when short of credit** (`:968`): it doesn't depend on how much credit is missing. This adds needless wakeups at low rates and up to 50 ms extra delay, including for the first chunk at unlimited rate (`:909`). This is latency, not incorrect totals.
- **Repeated work on each refill**: `refillSendCredit` looks up every queued job to recompute capacity, twice per chunk. It's correct but could be cached when jobs are added or removed.

## Checked and correct

- **Rate math:** units (bytes/s × ms = milli-bytes) and overflow limits (rate ≤ 2³⁰, × 1000 is fine in `qint64`) are correct.
- **Burst size:** credit never goes negative, because the check at `:949` covers the charge at `:1006` and the second refill can only add. The burst stays within the largest queued chunk plus the tag plus `rate/20`.
- **Timer state:**
  - The current job stays queued during callbacks, so cleaning up another job can't stop the timer.
  - Stopping the timer when the queue empties also resets credit. Restarting it in `buildAndSendManifests` re-arms the clock.
  - `refillSendCredit` is never called with an unstarted clock.
- **Lifetimes and cancellation:** the job pointer copy at `:940` keeps it alive. `sendNextChunk` checks the state after the chunk callback, the progress update and each completion frame. Re-adding a job to the queue requires both identity and state to match. Aborts arriving during the handshake or the credit wait behave correctly.
- **`setConfig` threading:** it is queued and runs in the engine's thread (`FileTransferManager.cpp:394`).

## Existing gaps (not introduced here, not claimed fixed)

- **Server budget:** there is no negotiation or refusal. The limit is local and counts ciphertext only, not control frames or protobuf/transport framing.
- **Recipient receipts:** nothing confirms delivery, so "Saved" on the sender means all chunks were sent, not received.
- **Downstream backpressure:** the 32-chunk / 4 MiB batch limit only bounds the engine's own turn. The queues in `Transport::queueChunk` / ServerHandler are still unbounded, especially at unlimited rate.

## Test coverage note

The rate tests only check that sending never exceeds the rate. No test checks minimum throughput, changes `setConfig` mid-transfer, or covers the chunk size vs. idle timeout case, which is why both defects above pass all nine new tests.
