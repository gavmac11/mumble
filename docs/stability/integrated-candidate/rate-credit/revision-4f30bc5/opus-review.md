## Review of sender-credit revision 4f30bc577 (read-only)

**I found nothing that must be fixed in the three corrections.** I only read the code: I didn't build it or run the tests, so the "all 14 pass" result is yours, not something I checked.

### The three corrections

**1. Restart rounding: fixed.** See `FileTransferEngine.cpp:929-954`.
- The clock now runs continuously. `m_sendCreditLastNSecs` is a cursor, so calling refill very often loses no time, even on a coarse clock.
- The units work out: rate × ns ÷ 10⁶ gives thousandths of a byte. The leftover `% 1000000` is carried into the next refill.
- **Overflow:** the manager caps the rate at 2³⁰ (`FileTransferManager.cpp:390`). Even the full quint32 range times the 1e9 ns cap is about 4.29e18, below the qint64 limit of 9.22e18. Adding the remainder (under 10⁶) changes nothing. `capacity*1000` is about 1e11 at most.
- **Clock and cursor stay in step:** the clock is only restarted at `:104` and `:924`, and both reset the cursor to 0. When `setConfig` resets the cursor but leaves the clock stopped (`:101`), nothing can refill before `:924` restarts it, because the queue is non-empty only while the timer is active. `qBound(0, …, 1s)` keeps the elapsed time non-negative, and a stalled thread can only lose credit, never gain it.
- **Credit stays between 0 and capacity:** the check at `:975` needs at least `min(chunk, remaining)+Tag`, and `:1032` subtracts `ciphertext.size()`, which is the same amount for an unchanged file. The refill at `:1030` only adds. At the cap, the remainder is zeroed (`:951`), so it can't leak a hidden extra byte. If capacity shrinks when a large-chunk job ends, the next refill clamps credit down to it.

**2. Same-rate `setConfig` reset: fixed.** See `:95-105`.
- Credit is only reset when the rate actually changes. Pin-cache refreshes (`FileTransferManager.cpp:357`) no longer starve sends.
- If the rate changes from unlimited to a limit mid-transfer, credit starts at 0. That is the cautious direction.

**3. Low-rate chunk size: fixed for new jobs.** See `:686-692`.
- At the UI's minimum of 1 KiB/s, the chunk is the 16 KiB floor, which takes about 16 s, under the 60 s idle timeout.
- Capacity always holds at least one of the largest queued chunks plus `rate/20` (`:940-945`). So the job at the front of the queue can always eventually send, even after the rate is lowered: no deadlock.

### Re-entry and timer lifetime: no defects found
- `paceSends` holds a `shared_ptr` to the job (`:966`), so the job survives a `finishSend`/`cleanupSend` triggered from inside a callback.
- The job stays queued while its callback runs (`:980-987`). So cleaning up another job can't empty the queue and stop the timer, and the job is only re-queued if it is still the live, transferring entry.
- If cleanup happens without a state change, running `finishSend` twice is harmless: `file` is null-checked and the map removal does nothing the second time.
- The `unique_ptr<QTimer>` with `this` as parent is safe: the member is destroyed before `~QObject`, and the timer unregisters itself from its parent.

### Remaining risks (none are must-fix)
1. **Chunk sizes that aren't a multiple of 1 KiB (medium; can't check from this snapshot).** `rate/4` with rate = n KiB gives n×256 bytes, e.g. 101 KiB/s gives a 25,856-byte chunk. The old UI only ever produced multiples of 1 KiB, and no test row produces anything else. Receiver-side manifest checks (`FTManifest`) and older peers aren't in these files. Confirm they accept any value in [16 KiB, 1 MiB], or round `rateChunk` down to a multiple of 1 KiB.
2. **Debt forgiven on re-entry (low).** If a `m_transportChunk` callback calls `setConfig` with a different rate, the debt charged at `:1032` is wiped, allowing one extra chunk. Today `setConfig` is queued (`FileTransferManager.cpp:394`), so this only happens if a callback runs a nested event loop.
3. **Busy loop during shutdown (low, bounded).** `:963-964` returns without touching the timer. If the last interval was 0 ms, it keeps firing until the thread's event loop exits.
4. **Hashing blocks the engine thread (out of scope).** `merkleRootFromDevice` in `startSend` (`:703`) runs on the engine thread. Hashing a very large new file pauses sends already in flight. Credit is capped at 1 s so there's no burst afterwards, but receivers' idle timers keep running.

### Known gaps this revision does not solve
- Server-side negotiation and receipts.
- GUI and TLS queues: `paceSends` only limits the engine's own dispatch (`:958`).
- Fairness between existing jobs with different chunk sizes: the front-of-queue `break` at `:977` makes smaller jobs wait.
- Timeouts with several concurrent very-low-rate jobs: round-robin at 1 KiB/s with 4 or more jobs puts more than 60 s between chunks for each job.
- Lowering the rate after a manifest has been sent can't change that job's chunk size. Capacity prevents a deadlock, but not a receiver timeout.
