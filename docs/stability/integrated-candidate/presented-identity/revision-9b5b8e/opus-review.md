**No Must-fix issue remains.** Both earlier Must-fix issues (M1 and M2) are fixed in this snapshot, and I found no concrete regression from the correction. Several Should items and acceptance gaps are still open, listed below.

This was a read-only review. I didn't build or run anything and used no network. I couldn't check the `source.json` hashes or head `9b5b8e26…` against the files, because there's no git repo here and no way to hash files with these tools. The three test logs are your output; I read them but didn't reproduce them. No plan file was written, since this was review only.

## Prior Must-fix issues

**M1 (sender read its expected key from the mutable cache): fixed.**
- `FileTransferEngine::startSend` (`FileTransferEngine.cpp:783-806`) builds each initiator session from the session → key map it is given. The send path no longer reads the cache; only the receive path (`:457`) still does.
- A key of the wrong length rejects the whole job.
- The session rejects a key that doesn't match a non-empty expected key during the handshake (`FileTransferSession.cpp:137`, `:265`). No first-use dialog fires, and no manifest or chunks are sent to that peer.
- The test `selectedPinSurvivesCacheRemoval` (`TestFileTransferEngine.cpp:881-907`) covers the original attack: it uses real crypto, an emptied cache, and Bob presenting a substitute key. Per your logs it failed with 1 chunk before the fix and passes after.
- An empty key now only comes from `sendRecipient` after a successful database read that found no pin (`FileTransferManager.cpp:266-276`). A database error produces no recipient instead of a first-use one. `senderFirstUseRequiresReadableDatabase` covers this.

**M2 (send dialog result not rechecked for name or trust): fixed.**
- `MainWindow.cpp:5009-5027` records a full snapshot of each recipient before the dialog opens.
- After it closes, `:5043-5054` checks each selected recipient again with `sendRecipientStillCurrent`.
- `FileTransferManager::startSend` checks again (`:300-304`) before queuing the key map, with no event processing in between.
- The comparison (`:284-290`) covers the user pointer, name, key, trust state, server, generation and channel; capability and feature-enabled are checked inside `sendRecipient`.
- Session reuse is caught: the old `QPointer` is null after deletion, so it can't equal the new user.
- The ten changed-context test rows plus the unchanged row check this shared function directly.

## No concrete regressions

- `startSend` has one production caller (`MainWindow.cpp:5064`), and it uses the new signature.
- The extra `refreshPinCache()` in `startSend` no longer affects sending.
- The new `querySucceeded` output in `PeerTrustStore::lookup` (`PeerTrustStore.cpp:102-123`) doesn't change existing callers, because its default is `nullptr`.
- One behaviour to be aware of, not a defect: the abort after a changed key at M4 (`FileTransferManager.cpp:107-109`, `:125-128`) uses `abortTransfer(transferId)`, which cancels **every** recipient of that send (`FileTransferEngine.cpp:1194-1209`). That fails safe, but it should be documented or done per recipient.

## Should-fix

1. **New, specific: the sender's first-use pin is filed under the user's current name, not the name captured at send time.**
   - The M4 handler checks and pins under `user->qsName` as it is when the GUI processes the event (`FileTransferManager.cpp:106`, `:125`). The engine doesn't carry the captured `SendRecipient.name`.
   - Scenario: you send to a first-use peer. Before M4 is processed, the server renames that session to a name with no pin. That name then gets a Pinned entry for a key you never chose for it. Later send dialogs show that name as Pinned, not New.
   - This doesn't defeat Verified, and you've documented first use as TOFU, so it isn't a Must. But it contradicts the snapshot model this correction introduced.
   - Fix: send the captured name and trust state with the job and use them in the event. Pin only if the live name and user still match the snapshot; otherwise abort that recipient.
2. **Untested: the post-M4 changed-key abort.** There's no Manager test showing that a pin appearing for a first-use name between capture and M4 queues `abortTransfer(pendingTransferId)` and stores nothing. Likewise, no test drives `FileTransferManager::startSend` itself; the tests call `sendRecipientStillCurrent` directly.
3. **Your earlier Should list is unchanged and still open:**
   - atomic name/key lookup on the receive side;
   - connection tags on inbound messages;
   - scoped cancel for stale dialogs;
   - missing-pin messages and the `Aborted` update overwriting `Failed`;
   - trust recheck on manual save;
   - the sender verification lifecycle. `resume(false)` still resolves the parked handshake by session only, so a sender-side decline or a non-current check (`:101-104`, `:111`, `:129`) can drop an unrelated parked receive handshake from the same peer. It also still doesn't stop the send, and the first-use manifest still goes out before the GUI sees anything, as you said;
   - re-verification flow (`removePin` still deletes without matching the fingerprint or checking the row count, `PeerTrustStore.cpp:92-100`);
   - receive-side storage errors (`check` still treats a lookup failure as `NewPeer`, `:45`);
   - re-entry from `applyEngineConfig` → `disconnectCleanup` when the feature is disabled;
   - case-sensitivity of names.

   This review doesn't qualify any of them.

## Test notes

- The two receive decision fixtures now assert `m_signCalls == 1` (`TestFileTransferManager.cpp:112`, `:135`). A signing attempt only happens on `resume(true)`, so they now distinguish resume(true) from resume(false). That earlier test gap is closed.
- The focused Engine log covers only `selectedPinSurvivesCacheRemoval` and `firstContactPinFlags`. The ordinary round-trip tests weren't in the logs I was given, so I can't confirm they still pass.

## Acceptance gaps

- The full exact-source Mac build and all tests, and later Windows/Linux qualification, are still pending. The hashes and head are unchecked.
- No real-crypto test goes through `FileTransferManager` or `MainWindow`. The send-dialog recheck and the M4 abort have no GUI-level or end-to-end test.
- Should items 1 and 2 above, plus the earlier Should list.
- Not assessed, as you asked: shipping, media, GUI and final-hosting qualification, and the other open gates you listed earlier.
