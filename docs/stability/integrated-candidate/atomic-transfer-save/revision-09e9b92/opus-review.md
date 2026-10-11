I found no new must-fix in `09e9b92a2`. The corrections you describe are present in the code and in the right order: sync and check, then publish, then remove the stage, then sync the directory or reopen and commit, and only then `Saved`. My Qt-internals points below are from memory of Qt's sources, not checked against your Qt version. I only read the code; I did not build or run anything.

**Caveats:**
- The previous review's text isn't in this directory (only the 3 sources, `prompt.txt` and `source.json`). I checked the code against the corrections as you described them, not against that review's wording.
- I also didn't write a plan file or call ExitPlanMode, because those tools weren't available. This answer is the whole review.

## Previous must-fix areas vs. this commit

| Area | Status | Where |
|---|---|---|
| Native sync checked before publishing (because QSaveFile ignores sync errors) | Fixed. `flush()` and `fsync` (with EINTR retry) or `_commit` are checked before `commit()` or `create_hard_link`. | `FileTransferEngine.cpp:40-52`, `:1246`, `:1257` |
| Never overwrite the original in place | Fixed. `setDirectWriteFallback(false)`. | `:1245` |
| Create-only save refuses a target that appears during the copy | Fixed. The hard link fails with "already exists". | `:1269` |
| Existing symlink destination | Fails closed. A dangling symlink also fails in the create-only path. | `:1218` |
| Publication synced before `Saved` and before the receive is cleaned up | Fixed. `emitSaveDone` is reached only after the publication sync succeeds. | `:1284-1291`, `:1437-1449` |
| Failure after publishing: no false "original unchanged", receive kept | Fixed. Ready plus a specific warning; `cleanupReceive` isn't called. | `:1280-1288` |
| Stage removal checked | Fixed for the success path. | `:1274`, `:1280` |

## Accuracy corrections (not must-fix)

1. **"Checks stage close error" overstates what `:1268` does.** As I recall Qt's internals, `QTemporaryFile::close()` doesn't close the file at the OS level; it only flushes and seeks to the start. The real close happens inside `stage.remove()` at `:1274`, after publishing, and its error is ignored. So `stage.error()` at `:1268` only re-checks a flush that `syncSaveFile` already did. Because the sync was checked first, this doesn't affect safety, but the wording should be "flush/sync checked", not "close checked".
2. **macOS `fsync` is not a flush to the physical disk.** That needs `F_FULLFSYNC`. The "nine pass on Mac" result only shows the `fsync` return value was checked. That fits your "no claims beyond checked OS APIs" rule, but the wording should say so explicitly.
3. **"No stage leftovers before publication" is shown by tests, not checked in code.** Before publishing, the stage is removed by the QTemporaryFile destructor (end of scope at `:1275`), and that removal's result isn't checked. On Linux, Qt may create the stage as an unnamed file and only give it a name when `fileName()` is called at `:1258`, which actually helps with crash leftovers.

## Should-fix / risks I couldn't verify

4. **New files get different permissions depending on the path (POSIX).** A create-only save publishes the QTemporaryFile, which is created `0600`. A confirmed replacement copies the original file's permissions, and a confirmed save to a new path uses the normal umask default. Either make this consistent or document it (owner-only may be intentional for privacy).
5. **The `!stageRemoved` branch (`:1280-1283`) has no test.** Nothing injects a stage-removal failure. In that branch the directory sync is also skipped, so the warning is correct but nothing was synced.
6. **Test gaps in `TestFileTransferEngine.cpp`:**
   - The two after-publication rows don't check for stage leftovers; that check at `:492-495` only runs before publication.
   - The before-publication rows only inject at `m_syncSaveFile`. Nothing injects a `create_hard_link` failure or a non-NoError `stage.error()`.
7. **Windows is untested:**
   - `create_hard_link` runs while QTemporaryFile still has the stage open (see item 1). Whether `CreateHardLinkW` hits a sharing violation depends on how Qt opens the file. If it does, the create-only path always fails closed on Windows: safe, but not usable.
   - `syncSavePublication` opens the published file read-write (`:58-59`). A read-only attribute, or another process holding the file without write sharing, would trigger the after-publication warning even though the save worked.
8. **Retrying after a published create-only save needs confirmation.** A retry with `replaceConfirmed=false` hits "already exists" and shows the generic "Could not save the file". The warning at `:1287` says data is kept for retry but not that replacement must be confirmed. The test at `:499` passes `true`, which is the right contract, but the GUI needs to follow it, and GUI behaviour isn't qualified.
9. **Symlink race in the replacement path.** There is a short gap between the `isSymLink()` check (`:1218`) and `QSaveFile::open` (`:1246`), and QSaveFile resolves symlinks when it opens. This falls under your "shared writable directory not qualified" limit. After open, a swapped-in symlink is replaced by the rename rather than followed.

## Unchanged limits (agreed, not re-litigated)
A named stage can remain after a forced crash. Ownership-by-name in a shared writable directory, the Windows directory journal and power loss, disk full, GUI confirmation, and auto filenames/history/trust are all still open. Filesystems without hard links fail closed.

## Verdict
No new must-fix. Items 1–3 are wording fixes so the claims match the code. Items 4–8 are should-fix or need Windows/CI checking before any wider qualification. This review doesn't support any general durability claim, or any claim of stable commercial readiness.
