# Read-only Opus 5.5 review

Requested exact CLI model `claude-opus-5-5`; result metadata confirms that model. Review duration: 446.246 seconds. CLI was restricted to repository Read/Grep/Glob, with customizations/MCP disabled and no writes/commands. Production code remained at the integrated `740649f89` tree during the review; evidence-only commits followed.

These are reviewer findings, not accepted fixes or proof of release readiness. Code paths have been inspected; reproductions and regression tests will qualify each correction. The fatal handler uses `std::exit` and therefore skips the main-scope restoration guard; this is the first follow-up being reproduced. The Linux predictable shared temporary path also permits a local symlink/pre-creation hazard beyond the review's disclosure concern and requires private random owner-only storage.

I found **8 code defects**, **3 of them high-impact**, and **5 release acceptance gaps**. This was a read-only review: I edited nothing, ran no commands, and did not create a plan file. Line references are to the current worktree.

## Code defects

### High

**1. A second transfer from the same sender can be corrupted by chunks from a different transfer.**
- **Where:** `src/murmur/Messages.cpp:2705-2711`, `src/mumble/PQFileTransfer/engine/FileTransferEngine.cpp:33-35`, `:485-488`, `:1253-1256`, `:307-318`
- **What happens:** The server sends every file chunk to all file-capable users in the channel, not just the recipients. On the receiving side, if the receiver has no job matching a chunk's transfer ID, it falls back to that sender's not-yet-set-up job. That job buffers up to 8 chunks, and only the chunk index is stored, not the transfer ID.
- **Impact:** Sender A streams file X to B, then starts file Y to C. C buffers X's chunks, replays them into Y once Y's manifest arrives, and Y fails with a decryption or length error. Each stray chunk also restarts C's idle timer. Non-recipients use bandwidth and are counted against the server's aggregate limit.
- **Smallest fix:** Store the transfer ID with each buffered early chunk and drop any whose ID doesn't match the manifest when replaying. Separately, have the server relay only to recipients.

**2. With default settings, large transfers fail silently and the sender is told they succeeded.**
- **Where:** `src/mumble/Settings.h:471`, `src/murmur/Meta.cpp:71-72`, `src/murmur/Messages.cpp:2654-2656` and `:2696-2701`, `FileTransferEngine.cpp:947-957`
- **What happens:** The client sends at 4096 KiB/s (about 4.2 MB/s) by default. The server's default per-user file limit is 16 Mbit/s (2 MB/s), and the server-wide file limit is 64 Mbit/s (8 MB/s). Chunks over the limit are dropped with no notice. Once roughly 90 MiB has gone through the server's 360-slot measuring window, about half the chunks are dropped.
- **Impact:** The receiver never completes and times out after 60 s. The sender marks the transfer "Saved" as soon as its last chunk is queued, because there is no receipt from the receiver.
- **Smallest fix:** Set the default send rate at or below the server's per-user limit. Have the server send an abort/notice instead of silently dropping, or have the sender wait for a completion acknowledgement before showing "Saved".

**3. Nothing slows the client sender down, and slow but healthy receivers get disconnected.**
- **Where:** `FileTransferManager.cpp:126-143`, `FileTransferTransport.h:32-41`, `src/Connection.cpp:203-221`; the output bound is only set in `src/murmur/ServerUser.cpp:24`
- **What happens:**
  - Chunks go from the worker thread into the GUI event queue and then into the socket with no limit. If the send rate is set to 0, `budget = INT64_MAX` and the whole file is queued in one tick.
  - When the sender's upload is slower than 4 MiB/s, the socket buffer grows in memory and delays pings and control messages behind file data.
  - On the server, a receiver whose download is slower than about 2 MB/s builds a backlog beyond the 16 MiB bound. The server then cuts that user's entire session (voice included), not just the transfer.
- **Smallest fix:**
  - **Sender:** have the pacing timer skip a tick while the connection's queued bytes exceed a threshold.
  - **Server:** for file data only, drop chunks for a receiver above a lower soft limit, and keep the hard disconnect for everything else.

### Medium

**4. The pin cache goes stale, so pinned peers show up as "new", and a verification can attach to the wrong key.**
- **Where:** `FileTransferManager.cpp:352-381` and `:283-288`, `MainWindow.cpp:4939`
- **What happens:** The cache is refreshed only at server sync, on send, and on pin; it isn't refreshed on user join or channel move. A pinned peer who arrives later is treated as a first contact (`FileTransferEngine.cpp:387-395`), so a changed key shows a first-contact dialog instead of the "identity CHANGED" warning.
- **Impact:** If the user accepts, `pinPeer` takes the stored old fingerprint, not the one the user just checked, and marks that old pin verified. The engine then blocks the transfer anyway. The block itself is correct, but the stored trust state is now wrong.
- **Smallest fix:** Refresh the cache on user state and channel changes. In `pinPeer`, pin the pending presented fingerprint, and refuse if a different stored pin exists.

**5. On Linux, decrypted received files can be read by other local users.**
- **Where:** `FileTransferEngine.cpp:259-262`
- **What happens:** On Linux, `TempLocation` resolves to the shared `/tmp`. The directory `mumble-ft/<id>` and `content.bin` are created with umask permissions, typically 0755/0644, and that includes password-protected files. Another user can also pre-create `/tmp/mumble-ft` to block transfers. macOS and Windows temp directories are per-user.
- **Smallest fix:** Use a per-user app-data location, or `QTemporaryDir`, with 0700 on the directory and owner-only permissions on the file.

**6. One manual save can redirect auto-save to the home folder, where a trusted peer can drop dotfiles.**
- **Where:** `MainWindow.cpp:4882` and `:4839`, `FTManifest.cpp:230-232`
- **What happens:** A manual save overwrites the auto-save directory setting with whatever folder was chosen. If the user once saves into `~`, auto-accept then writes sender-chosen names there. Names like `.zprofile` or `.bash_login` are allowed; they only can't overwrite existing files. Windows device names (`NUL`) and `name:stream` (alternate data streams) also pass the filename check.
- **Impact:** Auto-accept is off by default, but when on, a verified peer can plant shell startup files.
- **Smallest fix:** Remember the last manual-save folder in a separate setting. For auto-save, reject leading dots, control characters, `:` and Windows reserved names.

**7. In production, the fail-closed database path skips the logger-handler restoration, and the test doesn't cover that path.**
- **Where:** `src/Logger.h:44-47`, `src/Logger.cpp:48-49`, `Database.cpp:99`, `main.cpp:440`, `TestDatabasePath.cpp:96`
- **What happens:** In production, `qFatal` goes through Mumble's handler, which calls `std::exit(1)`. That runs static destructors and Qt plugin teardown while `QApplication` and main's restore guard are still alive, and with Mumble's handler still installed. The prior handler is never restored, which is the same teardown hazard the logger repair targeted. The test child uses Qt's default handler and expects `CrashExit` (abort), so it never exercises this path.
- **Smallest fix:** For fatal messages in the Qt handler, flush spdlog and call `std::_Exit(1)`, or return and let Qt abort. Add a child test that installs Mumble's handler.

**8. A failed explicit database path exits with no message to the user.**
- **Where:** `Database.cpp:97-101`
- **What happens:** The general failure case at `:137` shows a `QMessageBox` before `qFatal`; this case doesn't. On packaged Windows or macOS the app just disappears.
- **Smallest fix:** Show the same critical dialog before `qFatal`.

### Low

- **Overwriting a saved file can never succeed.** `MainWindow.cpp:4880` passes `DontConfirmOverwrite`, so Windows and Linux don't ask. The engine (`FileTransferEngine.cpp:1063-1073`) then uses `QFile::rename`/`copy`, which never overwrite, so the result is "Could not save". macOS's native dialog confirms the replacement and then fails anyway. Fix: remove the flag, and remove the existing target after confirmation.
- **Restore readiness is inconsistent across the hosting tools.** `guest_state.py:253-256` and `deploy_guest.py:236-241` mark the guest `ready` after a 1-second sleep plus `is-active`. A server that crash-loops after that is recorded as ready, and resume is then refused. `historical_recovery.py:187-205` already does a pinned local TLS readiness check. Fix: reuse that check before saving `ready`.
- **Historical restore can lock out SSH.** `historical_recovery.py:289` hard-codes SSH port 22 in the firewall. A guest whose SSH runs on another port loses admin access. Fix: add `--ssh-port`, as `deploy_guest.py` has.

## Release acceptance gaps (not code bugs)

1. **The Cocoa repair can silently drop out of a release build.** `cmake/qt-macos-accessibility.cmake:6-12` returns with no warning if the build isn't static, the option is off, or Qt isn't exactly 6.10.0. A Qt bump would ship the crash. It should be a hard error for packaging builds. Separately, the missing accessibility row is still unqualified; the patch removes borrowed-ID deletion but does nothing to how rows are populated.
2. **Builds depend on the network.** The Qt source is fetched from GitHub codeload at configure time (`:16-18`); the hash check fails closed, but offline builds can't proceed. Restore steps run unpinned `apt-get` installs (`guest_state.py:233-234`, `historical_recovery.py:227-231`), so the "pinned historical binary" still depends on whatever library versions Debian 12 serves that day.
3. **The output-queue evidence models a stalled receiver, not a slow one.** Defect 3 shows a slow but working receiver can lose its session. Acceptance needs a test with a receiver below 2 MB/s and the default sender rate.
4. **Transfer delivery isn't measured end to end.** No test covers defects 1 and 2 (concurrent transfers from one sender; files larger than the meter window). The real-server E2E tests use small files.
5. **Already listed as open in your notes:** hosted combined candidate, signatures, native GUI checks on Windows 11 and macOS 15/27, physical media, endurance, provider RTO, and PR29 human review.
