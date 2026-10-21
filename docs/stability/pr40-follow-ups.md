# PR #40 agreed follow-ups

Status: open. Baseline: review round 2 at `4e9cf45cffdd99ee497e5c47157ee18b90688a69` (2026-10-11).
Source: the non-blocking follow-up review and replies in PR #40. All items are agreed
non-blocking changes for a separate focused follow-up; none gates this PR.

1. **Read-admission deadline tradeoff** (`src/Connection.cpp`, deferral path). The fixed 2 s
   congestion-episode deadline can fail an inbound transfer when engine-thread save work
   (copy + fsync) outlasts it while the budgets are full, and a held `FileData` frame
   head-of-line-blocks tunneled TCP traffic for the episode. Follow-up: measure slow-storage
   and tunneled-voice impact; consider save-aware admission or a deadline scaled to the
   transfer. Constraint: removing the deadline requires a different bound on how long an
   incoming transfer may hold the connection.
2. **`checkAndPin` confirmation edge** (`src/mumble/PQFileTransfer/identity/PeerTrustStore.cpp`,
   final `check()`). A transient read failure after a successful `INSERT` reports the stored
   pin as failed. Follow-up: distinguish persistence success from confirmation failure with
   injected read-failure coverage, preserving `Verified` state and changed-key rejection.
   Do not use `numRowsAffected() == 1 ? Pinned : check(...)`: it downgrades `Verified` to
   `Pinned` in the same-key verified race covered by `concurrentPinPreservesIdentity`.
3. **Mode-probe cleanup on Windows** (`src/mumble/PQFileTransfer/engine/FileTransferEngine.cpp`,
   probe removal). A failed `modeProbe.remove()` (sharing violation) aborts the save and can
   strand hidden `.mumble-ft-mode-*` files; retries accumulate them. Follow-up: best-effort
   cleanup or a bounded removal retry verified against a simulated sharing violation,
   keeping publication atomic and staging cleanup separate.
4. **Held-frame admission verdict caching** (`src/mumble/ServerHandler.cpp`, admission
   callback). The callback re-peeks and protobuf-parses the immutable held frame on every
   20 ms retry tick (~100 parses per episode); only the budget half of the verdict needs
   re-evaluation while a frame is held.
5. **Relay-bypass predicate deduplication** (`src/mumble/ServerHandler.cpp`, admission
   callback vs `handleMessage`). The "FileData ≤ 512 B parsing with
   `relay_accepted`/`relay_retry_after_ms`" predicate exists in two copies. Extract one
   helper so a frame bypassed at the socket layer can never be dropped as over-limit by
   `handleMessage`.
6. **Trust-cache recovery deduplication** (`src/mumble/PQFileTransfer/engine/FileTransferManager.cpp`,
   live-check path and the `handleControlMessage` M1 retry). Two copies of the single-peer
   cache update already differ slightly. Extract a shared `updatePeerCache` helper.
7. **Congestion-episode state** (`src/Connection.cpp`, read-admission state). Replace the
   `m_readPauseClock` / `m_lastReadDeferral` / `m_frameDeferred` trio and its compound
   restart condition with a single stored episode deadline. The current form already
   required the correction in `cb24200f4`.
8. **Transport wrapper cleanup** — resolved by the 2026-10-20 slop audit. The manager's
   two callbacks lock the captured connection and call `ServerHandler` directly; the
   forwarding header and tests of a separate fake queue implementation were removed.
