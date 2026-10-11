# Review: shared rate-credit pacing in `FileTransferEngine`

The credit math is correct, but I found one real defect: sending can stall permanently if a callback cleans up a different job while a chunk is being dispatched. There is also one regression in unlimited-rate throughput, and two smaller points. I only read the three files; nothing was built or run.

## What holds up

- **Aggregate rate is correct.** In `paceSends`, credit only grows with elapsed time, starts at 0 whenever the timer starts (`buildAndSendManifests`), and resets when the queue empties. Total sent stays at or below rate × time, and any window of length w sends at most `capacity + rate·w`. The old bug that gave every job a free chunk per tick is gone.
- **Late timers cause only a bounded burst.** Elapsed time is clamped to 1 s, and credit is capped at "largest chunk + tag + rate/20". A late timer, or a worker blocked by hashing or Argon2, can burst at most about one chunk plus one tick's worth.
- **Object lifetimes are safe.**
  - `paceSends` keeps a local `shared_ptr` to the job while sending.
  - It only re-queues the job if `m_sendJobs.value(id) == job` and the job is still `Transferring`.
  - `sendNextChunk` re-checks `lastState` after `m_transportChunk` returns, so a job that aborts itself from inside the callback is handled. `synchronousChunkAbortStaysAborted` covers this.
  - The `unique_ptr<QTimer>` that is also parented to the engine matches the existing pattern and is destroyed before the parent deletes its children.
- **Shutdown is handled.** The early `return` in the loop leaves the timer running, which is harmless while the thread exits.

## Defects introduced by the shared timer

1. **Sends can stall permanently (Medium–High).** This involves `paceSends` (`FileTransferEngine.cpp:949-954`) and `cleanupSend` (`:1034-1038`).
   - `paceSends` removes the job from the head of `m_sendPaceOrder` *before* calling `sendNextChunk`, and puts it back afterwards.
   - While only one job is sending, the list is therefore empty during the transport and `transferUpdated` callbacks.
   - If anything in a callback cleans up a different job (for example `abortTransfer(otherId)` on a job still handshaking), `cleanupSend` sees the empty list, stops the timer and zeroes the credit.
   - The original job is then put back on a stopped timer, and nothing restarts it. That transfer hangs forever, at any rate setting.
   - Today this needs callbacks that run synchronously, but the invariant is fragile.
   - **Fix:** rotate the job only *after* `sendNextChunk` returns (`removeOne` then `append`). Alternatively, after the loop, restart the timer and clock if the list is non-empty but the timer is stopped.

2. **"Unlimited" is no longer unlimited, and high rates are silently clipped (Medium).** This is the 32-chunk / 4 MiB per-tick cap in `paceSends` combined with the fixed 50 ms interval.
   - With rate = 0, throughput is now at most 640 chunks/s and at most 80 MiB/s. With 16 KiB chunks that is only about 10 MiB/s.
   - The test comment at `TestFileTransferEngine.cpp:518` ("drain in one tick") is now false.
   - With rate > 0, any configured rate above min(32 × chunk, 4 MiB) × 20 per second (e.g. 16 MiB/s with 16 KiB chunks) under-delivers. The credit hits its cap and the excess is thrown away without any signal.
   - **Fix:** when the loop exits because of the yield cap rather than lack of credit, schedule the next run with a 0 ms timer instead of waiting for the next 50 ms tick.

3. **`setConfig` now changes pacing state (Low–Medium, depends on the manager).** It resets the credit, restarts the clock and reads `m_sendPaceTimer->isActive()` (`:94-99`).
   - The header lists it with the direct setters, not with the "queued from the manager" calls.
   - If the manager calls it from the GUI thread while the engine runs on its worker thread, this is a data race and touches a `QTimer` from the wrong thread.
   - I couldn't see the manager code, so this needs checking. If confirmed, route the call as queued.

4. **Fairness is per chunk, not per byte (Low).**
   - Round-robin gives each job one chunk per turn. Jobs started under different `chunkSize` settings get bandwidth in proportion to their chunk size.
   - Credit capacity is sized by the largest chunk, so small-chunk jobs wait behind it at the head of the queue.
   - This is acceptable, but it should be documented.

## Test gaps and stale comments

- `simultaneousSendsShareRateLimit` only checks that both transfers delivered something. It doesn't check that their chunks actually interleave.
- The rate checks measure from the start of the test. They can't catch a short-window burst after a stall. Add a sliding-window check, and a test where a callback aborts another job while a chunk is being sent (this would catch defect 1).
- The comment "one 16 KiB chunk per tick" in `incomingAbortCleansUpSafely` (`:1079`) is stale. At 32 KiB/s, a chunk now goes out roughly every 0.5 s.
- `unlimitedSendYieldsToQueuedAbort` passes only because of the 32-chunk cap (it sees about 32 chunks, under its limit of 64). It's tied to the throttle in defect 2.

## Pre-existing gaps, not introduced by this patch

- **No server negotiation.** The rate is a client-side setting. It isn't checked against the Murmur server's bandwidth or message-size limits.
- **No delivery receipt.** The sender marks a transfer `Saved` once the last chunk and the `Complete` message are handed to the transport. Nothing confirms the receiver got them, and peer failures after streaming starts go unnoticed.
- **No backpressure.** The per-tick cap only limits how much the engine sends in one go. Queued signals to the ServerHandler and socket can still grow without limit, especially at unlimited rate. The receiver's password spool exists because there is no "ready" acknowledgement.
- **Not everything is counted.** Control frames (the manifest with its signature, Complete, Abort) and framing overhead are excluded from the budget. Server fan-out to several recipients is also uncounted, since each chunk is one upstream message.
- **Heavy work blocks the engine thread.** Merkle hashing in `startSend` and Argon2 in `buildAndSendManifests` run synchronously. They pause pacing and control handling, though the credit cap keeps the burst afterwards bounded.
