I found **two must-fix issues**, both in the send path. The receive-side fixes you described hold up in the code I read.

This was read-only. I didn't run anything, and I couldn't check the hashes in `source.json` or the commit `f337b951` against the files: there's no git repo here and no way to hash files with read-only tools. Everything below comes from reading the snapshot. I didn't write a plan file, since this was a review only.

## Must-fix

**M1. The sender's expected key comes from a cache that can change, and an empty entry accepts any key.**
- `startSend` refreshes the cache on the GUI thread. The worker reads the cache later, by session number, at `FileTransferEngine.cpp:789-797`.
- If that entry is empty, the initiator takes whatever key the peer presents (`FileTransferSession.cpp:137-142`).
- After M4 the engine sends the manifest right away (`FileTransferEngine.cpp:268-289`), so the file key is already wrapped before the GUI sees anything.
- The GUI check after M4 (`FileTransferManager.cpp:105-112`) only logs `peerBlocked`. `resume(false)` doesn't stop a send.
- The entry can go empty for a recipient whose pin is in the database in three ways:
  - they leave the channel or lose the capability between `startSend` and the worker's lookup;
  - they're renamed to a name with no pin;
  - the database lookup fails inside `refreshPinCache` (`:434`).
- The new refresh on user events (`Messages.cpp:867`, `:933`) makes this window wider. A malicious server can move or rename the recipient right after you press send, then sit in the middle of the handshake. That gets around a verified pin.
- **Fix:** on the GUI thread, record each recipient's session, name, expected key and trust state at `startSend`, and pass that to the engine. The engine should never read the cache for sender pins. Fail if a recipient who was Pinned or Verified now has no pin or a different one. Also abort that peer's send when the M4 check finds a changed key.

**M2. The file-send dialog's result isn't rechecked for name or trust.**
- `MainWindow.cpp:5038-5047` rechecks the user pointer, channel and capability, but not `qsName` or the pinned key.
- If someone is renamed while the dialog is open, a recipient shown as "Alice (verified)" gets the file encrypted to whatever key is pinned under the new name.
- This is the send-side version of the rename bug you just fixed for receiving.
- **Fix:** after the dialog closes, require that the name and key still match what was shown, and pass those values into M1's record.

## Should-fix

1. **Pin and name lookups aren't atomic.** The worker reads the pin (`Engine.cpp:457`) and the name (`:513`) under two separate locks. A refresh in between can mix two cache versions. That fails closed but blocks valid transfers. Return both values in one call.
   - The resume path (`Manager.cpp:92-96`, `116-117`, `Engine.cpp:1383-1395`) also re-reads the cache instead of using the key the GUI just approved. If the cache keeps missing, the same prompt can repeat.
2. **Incoming messages aren't tied to a connection.** `handleControlMessage` and `handleDataMessage` (`Manager.cpp:195-223`) post to the worker straight from the server thread with no connection tag. One posted just before a disconnect can run after the worker's cleanup and get the new connection number. Murmur reuses session numbers, so an old M1 could be shown as coming from a different user. Tag each message with its connection and drop mismatches on the worker.
3. **Dropping a stale dialog leaves things pending.** The early return at `MainWindow.cpp:4947-4949` leaves the GUI's pending entry and the worker's parked handshake in place. The parked handshake silently drops retries from that peer for up to 60 s (`Engine.cpp:468-470`). Add a cancel call scoped to the prompt token.
4. **Blocked-transfer messages are misleading.** A missing pin or missing name (state `NewPeer`) is reported as "identity changed" plus a CriticalError `peerBlocked` (`Manager.cpp:71-77`, `MainWindow.cpp:4958`). Also, the engine's later `Aborted` update overwrites the explanatory `Failed` card.
5. **Manual save doesn't recheck trust.** The trust check runs only when the transfer becomes Ready. Clicking save later (`MainWindow.cpp:4874-4895`, `Engine.cpp:1213`) saves even if the pin has since changed or been removed.
6. **The sender's first-contact dialog has no real effect.** Declining doesn't stop the send, and the pin stays. Its result is passed to the engine by session only (`Manager.cpp:308-310`), so it can resolve an unrelated parked receive handshake from the same peer. Pass the prompt type and token through, and either abort the send on decline or change the dialog wording.
7. **There's no flow to re-verify or replace a pin.** Nothing in production calls `removePin`, yet the block message tells the user to re-verify. When you add the flow:
   - match the fingerprint in the DELETE;
   - require exactly one row affected;
   - call `refreshPinCache` afterwards.
8. **Database errors look like first contact.** `PeerTrustStore::check` and `lookup` treat a database error as `NewPeer` (`PeerTrustStore.cpp:44-46`, `:111`). Every current caller fails closed except the cache rebuild, which feeds M1. Return a separate error state.
9. **Possible re-entry problem when the feature is disabled.** `refreshPinCache` → `applyEngineConfig` → `disconnectCleanup` (`Manager.cpp:413`, `458-461`) runs on every user event while the feature is off. It increases the connection number and clears pending prompts in the middle of a call. Today callers check `bFTEnabled` first, but that's fragile. Move the cleanup out of `refreshPinCache`.
10. **Minor:** `ft_pins.username` matching is case-sensitive. Check this against Murmur's name rules.

## Test coverage

- **No sender-side tests.** Nothing tests M4 observation pinning, a changed key at M4, or what `startSend` binds. M1 and M2 are both untested.
- **Two tests can't tell resume-true from resume-false.** `joinedMatchingPeerKeepsVerificationWithoutDialog` and `displayedKeyIsPinnedAndVerified` refuse signing, so the outcome looks the same either way. Use a signer stub that counts calls and returns false, and assert it was called once, or assert on the engine's parked and receive jobs.
- **The Ready test uses a synthetic update.** `incomingReadyUsesCapturedIdentity` emits a hand-built update. It doesn't combine the engine's name capture with the real manager cache, and it doesn't check the `Aborted` update that follows.
- **The real-crypto tests skip the production code.** The E2E test uses its own live-database pin lookup (`TestFileTransferE2E.cpp:63-70`). It bypasses the manager's cache, handlers, Ready check and `MainWindow`, so no real-crypto test touches the production trust logic.
- **Untested GUI paths:** the `Messages.cpp` refresh triggers, `MainWindow` dialog rechecks, the manual-save trust check, and the generation filter on inbound messages.
- The three negative logs reproduce the fixed receive-side faults on the fixture seam. They're consistent with the new tests, but no fault was reproduced on a shipping binary.

## Acceptance gaps

- Must-fix items M1 and M2 above.
- Execution evidence for this exact head: the Mac build and tests are still running, and the `source.json` hashes and commit are unchecked.
- Real-crypto tests through `FileTransferManager`, beyond the trust-decision fixtures.
- A re-verification flow.
- Plus the items you already listed as separately open: protocol, relay, receipts, rate limiting, installed GUI and media, OS and signing, recovery and commercial gates.
