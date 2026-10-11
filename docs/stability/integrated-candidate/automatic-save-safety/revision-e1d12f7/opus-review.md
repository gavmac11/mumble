Actual restricted read-only CLI model claude-opus-5-5; frozen sourcee1d12f7e1140975da419909594127da0e8fa0a70; duration142638ms. No reviewer test execution. The possible compilation concern already compiles in this Mac build; native Linux/Windows checks remain pending here. Warning is console/logger guidance, not a claimed visible one-time GUI notice.

I found no must-fix regression in the frozen sources. This was a read-only review: I used only Read, Grep and Glob, changed nothing, and built or ran nothing. I couldn't write a plan file because no Write or ExitPlanMode tool is available here, so the review is below.

**Scope:** `source.json` head is `e1d12f7e1140975da419909594127da0e8fa0a70`. The new pieces are the `JSONSerialization.cpp` marker logic (compared against `prior-JSONSerialization.cpp`), `SettingsKeys.h:233` and `TestFileTransferUi.cpp:165-206`. I also read the newly included engine, manager, manifest, constants, config and `Settings.cpp` load/save sources. For `MainWindow.cpp`, only the excerpt (lines 4801–4910) was available.

## Does the migration fix old redirected folders?

**Yes, and it fails closed.**
- **Load logic** (`JSONSerialization.cpp:195-204`): the check runs after all settings are loaded. Auto-save is only kept if `misc.file_transfer_auto_save_policy_version` is a JSON integer equal to 1. A missing key, `0`, `2`, `"1"`, `true`, `null`, `1.0`, or a missing or non-object `misc` all turn `bFTAutoAcceptPinned` off. `qsFTDownloadDir` is left alone.
- **Save logic** (`:101`): the marker is written every time, outside `PROCESS_ALL_SETTINGS`. Because it isn't a `Settings` field, the "skip default values" rule can't drop it. The default for `bFTAutoAcceptPinned` is `false` (`Settings.h:479`), so a disabled profile saves with the key left out and the marker present, and reloads disabled.
- **No path skips the check:**
  - The `.back` backup file and settings written by an older client have no marker, so they fail closed. An older client also writes a fresh JSON without the marker, so a downgrade followed by an upgrade disables auto-save again.
  - `legacyLoad` (QSettings or the registry) never reads file-transfer keys, so those profiles keep the `false` default.
- **Nothing else turns it back on:** the only writers of `bFTAutoAcceptPinned` and `qsFTDownloadDir` in the provided files are `from_json` and `FileTransferConfig::save` (`FileTransferConfig.cpp:40-41`). The settings page shows the folder right below the checkbox ("Automatically save files from verified contacts"), so turning it back on there is a real review step.
- **No regression for others:** profiles with auto-save off load unchanged, and current profiles that have the marker are unaffected.

## Must-fix

**None remain.**

## Should-fix

1. **Possible compile problem at `JSONSerialization.cpp:197`.** `misc->contains(SettingsKeys::FT_AUTO_SAVE_POLICY_VERSION_KEY)` passes a `SettingsKey` directly. That class has two conversion operators (`SettingsKeys.h:19-20`), and every other `contains` call in this file uses `static_cast< const char * >(...)` (`:163`, `:172`, `:229`, `:233`). That pattern suggests the uncast call may be ambiguous with nlohmann's overloads. I can't build to confirm. Use the same cast to be safe. `at()` without a cast is already used at `:230`, so `:198-199` are fine.
2. **Users aren't told that auto-save was turned off.** The `qWarning` only reaches the console or log file. Affected users will just see files stop saving automatically. Show a one-time message in the Mumble log, or a note on the File Transfer settings page.
   - **Minor:** "one-time" really means "until the next settings save". A crash before the first save logs the warning again. That's harmless, but describe it accurately.
3. **Test gaps in the migration test:**
   - Nothing checks that *no* warning appears for a current profile (marker 1, auto-save on). QTest doesn't fail on unexpected warnings unless `QTest::failOnWarning` is set, so the round trips at `:149`, `:202` and the `manualHistory…` test don't cover this.
   - There are no rows for `1.0`, `null`, or a missing `misc` object.
   - Nothing covers loading the `.back` file.
4. **A newer profile loses its marker after a downgrade.** If a future client writes marker `2`, this client disables auto-save and saves `1` over it. That fails safe, but document that later versions must treat `1` as "reviewed under policy v1", or re-check it themselves.
5. **Carried over from the prior review:** `rememberManualSaveDirectory` runs before the result of a queued save is known (excerpt `:94`). A failed manual save still updates the remembered folder. This doesn't affect automatic saving.
6. **Downgraded from the prior review — the raw suggested filename in the Save dialog.** The manifest now visibly rejects `/`, `\`, `.`, `..`, empty names and names over 255 bytes (`FTManifest.cpp:227-232`, before the signature check). So `../` can't move the dialog to another folder. Odd suggestions like `C:x` are still possible, but the user confirms the save. Optional: use `QFileInfo(...).fileName()` for the suggestion.
7. **Optional:** `FileTransferConfig::save` accepts an empty or relative folder with auto-save enabled. `automaticSaveTarget` then quietly refuses to save. Validating or warning in the settings dialog would avoid confusion.

## Earlier acceptance gaps, re-checked

- **Create-only default — now verified.**
  - `replaceConfirmed = false` is the default in the manager and engine headers.
  - The automatic path saves to a random staging file in the destination folder (`QTemporaryFile`), syncs it, then publishes it with `create_hard_link`. The hard link refuses an existing target, even one created during the copy. Symlink targets are refused (`FileTransferEngine.cpp:1218`, `:1252-1275`).
  - Replacing only happens on the manual path (`:1242-1251`, `QSaveFile` with no direct-write fallback).
- **Publication and save states — now verified.**
  - `Ready` is emitted once, at `:708`.
  - On success, the state becomes `Saved` and the job is cleaned up (`:1437-1448`).
  - On failure, `emitSaveFailed` re-emits `Ready` *with an error* while `lastState` stays `Ready`. MainWindow's automatic branch skips anything with an error, so there's no retry loop, and a manual retry still works.
  - If publishing succeeds but removing the staging file or syncing fails, the user gets a distinct error and the received data is kept.
  - The manager queues `saveTransferAs`, so the engine can't re-enter it, and the engine's `lastState == Ready` guard stops a duplicate automatic and manual save.
- **Upstream basename validation — now verified** (see should-fix 6). `MaxFileNameBytes = 255` (`PQFTConstants.h:96`) matches the tests. The automatic name filter is a strict superset of the manifest rules.
- **Other writers of the configured folder — verified within the provided files, still open for the rest of `MainWindow.cpp`.** The manager, engine, config, settings and excerpt don't write `qsFTDownloadDir` outside `FileTransferConfig::save` and `from_json`. Lines outside 4801–4910 are unreviewed.

## Remaining acceptance gaps

- Whether `:197` compiles (should-fix 1), and whether any of the tests have passed. I didn't run them.
- Writers of the folder elsewhere in `MainWindow.cpp`. There's no integration test for `onFileTransferUpdated` or `onFileCardClicked`.
- No GUI test of `FileTransferConfig` loading a migrated profile and turning auto-save back on. The test changes the `Settings` field directly.
- Full path length (Windows `MAX_PATH`), plus drive-relative `C:` and UNC paths in `QDir::isAbsolutePath`. These can only make saves fail, not make them unsafe.
- **Residual limits, as you scoped them:**
  - The freshness of the trust cache and presented key belongs to the separate open work order. `trustStateFor` reads the store by name and server digest at update time.
  - Physical power loss, filesystems without hard-link support (those fail closed and stay `Ready`), shared or synced destinations, a symlinked *configured folder*, and installed GUI/media are not qualified.
  - Saved files aren't checked for content: names like `.lnk`, `desktop.ini` or a double extension are still allowed into the configured folder.
  - Names outside the Basic Multilingual Plane (emoji, etc.) still fail closed by design.
  - A folder an older client redirected while auto-save was *off* stays configured. It's only protected by the user looking at it when they enable auto-save.
