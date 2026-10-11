I found one must-fix issue. The core ownership changes look correct otherwise. This was read-only: I didn't run anything, so the Qt behaviours below are from what I know of Qt, not from testing.

## Must-fix

**1. The new-file path publishes data that isn't on disk yet, then deletes the only other copy** (`FileTransferEngine.cpp:1212-1229`, `1377-1388`, `1128-1129`)

- **What happens:** the stage is written with `flush()` (`:1212`), closed (`:1214`) and hard-linked (`:1223`). `flush()` only hands Qt's buffer to the OS; nothing forces the data to disk. `emitSaveDone` then calls `cleanupReceive`, which runs `tempDirectory.reset()` and deletes the received file.
- **Consequence:** after a crash or power loss, the target name can exist but be empty or partly written, while the Ready source is already gone. This is the auto-save path, so it's the one users hit most. It undercuts the patch's "repairs transfer-save loss" goal.
- **Second gap:** the result of `stage.close()` is never checked. On NFS, which does support hard links, write errors like ENOSPC or EDQUOT can first show up at close or fsync. So `committed = !error` (`:1224`) can be true for a file that is short.
- **Fix:** after `copyReceived`, sync the stage before linking. On POSIX that's `fsync(stage.handle())`; on Windows, `FlushFileBuffers` on the OS handle from `_get_osfhandle(stage.handle())`. If the sync fails, fail the save. Also check `stage.error()` after `close()`. On POSIX, fsync the destination directory after the link and before `emitSaveDone`. The directory fsync applies to the `QSaveFile` branch (`:1203`) too: as far as I know `commit()` syncs the file but not the directory rename.

## Should-fix

**2. On Windows, a second plaintext copy can be left behind silently** (`:1211`, `:1225-1226`)
- After a successful link, the stage name is removed only by `~QTemporaryFile`, which ignores failure. If an antivirus scanner, indexer or sync client has opened the new file without delete sharing, `DeleteFileW` fails.
- The stage name then stays as a second link to the same plaintext, and Saved is still reported. A user who later deletes the saved file still has the data on disk. On Windows a leading `.` doesn't hide the file either.
- Fix: after linking, call `QFile::remove(stagePath)` yourself, check the result, and retry or log a warning.

**3. Leftovers after a crash or hung shutdown**
- If the process dies during the copy, `.mumble-ft-save-XXXXXX` files with plaintext stay in the user's download folder. On Unix they are hidden and nothing sweeps them up. That conflicts with the "shutdown removes unsaved plaintext" promise at `:1110-1111`.
- I would not add a sweep that deletes by pattern, since that breaks the "only remove what we own" rule. On Linux, a better route is an `O_TMPFILE` stage in the target directory, then `linkat(fd, "", …, AT_EMPTY_PATH)`. There is no stage name at all, and an existing target still makes it fail. Otherwise, document this as a known limitation.

**4. A narrow window where the stage is owned by name only** (`:1214` → `:1223` → end of scope)
- Once the stage is closed, the code is trusting a name, not an open handle. A local process that can write to the destination folder could swap the stage file. The link would then publish the swapped content, and the destructor would delete a file the engine doesn't own.
- This only matters for shared, writable download folders. It's low severity, but worth a comment; the `O_TMPFILE` approach in #3 removes it.

## Checked and correct

- **Ownership:** only the random, exclusively created stage is ever removed. The target and any unrelated `.part` file are never touched. Without confirmation, an existing target or a dangling symlink makes the link fail, so nothing is overwritten. If a file appears during the copy, the save also fails safely.
- **Replace path:** `setDirectWriteFallback(false)` is set (`:1200`). Any failure calls `cancelWriting()` (`:1205`), and a failed commit leaves the original in place. On Windows, a target that is open in another app or read-only makes the replace fail, leaving the original as it was.
- **Retry:** `emitSaveFailed` leaves `lastState` at Ready (`:1391-1401`), so a retry works. Success goes through `emitSaveDone` → `cleanupReceive(jobPtr)`, and the job is kept alive by the shared pointer.
- **Flag forwarding:** the flag defaults to `false` in `FileTransferEngine.h:106` and `FileTransferManager.h:64` and is captured by value (`FileTransferManager.cpp:242`). Auto-save (`MainWindow-save.cpp:32`) therefore always uses the create-only path.
- **GUI confirmation:** the dialog suppresses its own overwrite prompt (`:72`). The box is plain text, defaults to No, and Esc counts as No (`:76-83`). The generation is rechecked after both modal dialogs (`:73`, `:86`). `info` is a copy (`:61`), so the nested event loops can't leave it dangling.
- **No infinite auto-save retry:** a failure emits Ready with a non-empty error, which the `info.error.isEmpty()` check (`:26`) blocks.
- **Cancellation:** `copyReceived` checks for shutdown between 256 KiB blocks. The worker never re-enters itself during a save.

## Lower-priority notes

- **Symlink targets:** `QFileInfo::exists` follows symlinks (`MainWindow-save.cpp:75`). If the confirmed path is a symlink, `QSaveFile` (as far as I know) replaces the file it points to, which may be outside the folder shown in the prompt.
- **Permissions:** files from the create-only path get `QTemporaryFile`'s 0600 permissions, where the old `.part` path used normal umask permissions. That may be fine for privacy, but it's a behaviour change.
- **Silent no-op:** `saveTransferAs` returns without any event when the job is missing or not Ready (`:1167-1169`). If auto-save finishes while the manual dialog is open, the manual save does nothing and nothing tells the user.
- **A copy that outlives a disconnect:** `disconnectCleanup` doesn't interrupt the worker, so a large copy carries on. Its Saved event is then dropped by the generation filter, and the card can stay Ready even though the file was saved.
- **Test gaps:**
  - `saveDoesNotDeleteUnrelatedPart` doesn't assert that no stage file is left after the failed link.
  - `confirmedSaveReplacesAtomically` only checks the content. It never fails a replace partway to prove the original survives. Its `.mumble-ft-save-*` check (`:406`) can't see `QSaveFile`'s own temp names.
  - There's no GUI test of the confirmation flow.

The open issues you listed (manual-save history changing the auto-save folder at `:88`, unsafe auto-chosen names at `:31`, trust and key freshness at `:29`) are still there, and this patch doesn't touch them.

I couldn't write the plan file because no write tool is available in this session, so the review is only here.
