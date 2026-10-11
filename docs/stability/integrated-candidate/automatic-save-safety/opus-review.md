Actual restricted read-only CLI model: claude-opus-5-5. Source30fa66b21914b82ea048afdd5cfd5122cbd5d70f; duration97083ms. No test execution by reviewer. The old-profile migration concern is addressed by follow-up sourcee1d12f7, whose separate qualification is pending.

I found no must-fix regression in the frozen sources. This is a read-only review: I used only Read, Grep and Glob, modified nothing, and ran no tests. I couldn't write a plan file because no Write or ExitPlanMode tool is available here, so the review is below.

**Scope:** `FTSavePolicy.h`/`.cpp`, `Settings.h`, `SettingsKeys.h`, `SettingsMacros.h`, `PeerTrustStore.h`, `TestFileTransferUi.cpp`, and `MainWindow-save-excerpt.cpp`. Per `source.json`, the excerpt is `MainWindow.cpp` lines 4801–4910. The rest of `MainWindow.cpp` and `FileTransferEngine.h` (which holds `MaxFileNameBytes`, `FTTransferInfo` and `saveTransferAs`) are not in the frozen set.

## Must-fix

**None found.** I couldn't reproduce any way for a trusted peer to plant a dotfile, a Windows device name or an alternate data stream through the automatic path. Nor could I find any path where a manual save changes `qsFTDownloadDir`.

What I checked:
- **Name rules** (`FTSavePolicy.cpp:11-33`):
  - Empty names and names over `MaxFileNameBytes` (counted in UTF-8 bytes) are rejected.
  - Rejected at the start: `.`. Rejected at either end: any `isSpace()` character, which includes U+00A0 and U+3000. Rejected at the end: `.`.
  - Rejected anywhere: `/\:<>"|?*`. Rejecting `:` blocks alternate streams and `C:relative`.
  - Rejected anywhere: control and format characters (Cc, Cf, which covers NUL, DEL, bidi controls and the BOM), surrogates (Cs) and line/paragraph separators (Zl, Zp).
  - **Device names:** the stem is the text before the first `.`, trimmed and uppercased. That catches `NUL .txt`, `CON.log`, `CONOUT$.txt`, and `COM`/`LPT` followed by 0–9 or ¹²³.
  - **Unicode case mapping:** `toUpper()` can only cause extra rejections, never extra acceptances.
- **Eligibility gates** (`FTSavePolicy.cpp:35-42`): auto-accept must be on, the download folder must be set and absolute, and the transfer must be incoming, `Ready`, error-free, from a `TrustState::Verified` peer, with a safe name. `Pinned`, `NewPeer` and `Changed` peers are refused.
- **Manual history is kept separate:**
  - `rememberManualSaveDirectory` only writes `qsFTManualSaveDir` (`FTSavePolicy.cpp:52-54`).
  - The new key is defined at `SettingsKeys.h:232`.
  - It is serialized at `SettingsMacros.h:20`, and the `FILE_TRANSFER_SETTINGS` group is included in both `PROCESS_ALL_SETTINGS` (`:335`) and the `_WITH_INTERMEDIATE_OPERATION` variant (`:364`).
  - Old profiles without the key fall back to the download folder, then home (`:44-50`).
- **MainWindow:** the automatic path calls `saveTransferAs(id, target)` with no replace flag (excerpt `:41`). The manual path only calls `rememberManualSaveDirectory` (`:94`), after the dialog, the replace confirmation and a connection-generation check.

## Should-fix

1. **Profiles already affected by the old bug are not repaired.** Before this fix, a manual save rewrote `qsFTDownloadDir`. An existing profile may therefore have auto-save pointing at, say, `~/Library/LaunchAgents` or a Windows Startup folder. The dotfile rule doesn't protect those folders, so a verified peer could still put `x.plist` or `x.lnk` there. The legacy test (`TestFileTransferUi.cpp:148-153`) deliberately keeps the old value.
   - **Reproduce:** load a profile where `file_transfer_download_dir` is the Startup folder, `file_transfer_auto_accept_pinned` is true and `file_transfer_manual_save_dir` is missing. Receive `run.lnk` from a verified peer. It is saved into Startup.
   - **Smallest fix:** when loading, if the manual-save key is absent and auto-accept is on, turn auto-accept off (or require the folder to be reconfirmed), and log or notify the user once. I can't call this a must-fix because we can't tell a bug-written value from one the user chose, and I can't see the old code.
2. **The suggested filename in the manual Save dialog is built from the raw peer name** (excerpt `:76`). `baseDir + "/" + info.fileName` with `../../x` opens the dialog in a directory the peer chose. The user still confirms, and this isn't a regression. **Fix:** use `QFileInfo(info.fileName).fileName()` for the suggestion.
3. **The manual folder is remembered before the save result is known** (`:94` runs before `:95`). A failed save still updates the remembered folder. It's harmless to automatic saving, but moving the call after a successful save is cleaner if the API reports success.
4. **The setting name `bFTAutoAcceptPinned` is misleading.** It actually requires `Verified`, as the comment in `Settings.h:478` says. Renaming the field is optional, but the user-facing label should say "verified".

## Acceptance gaps (not verifiable from the frozen files)

- **Create-only default:** I can't confirm that `saveTransferAs(id, target)` defaults to "don't replace" and never overwrites, because `FileTransferEngine.h` isn't included. The automatic path depends entirely on that.
- **Repeat updates:** `onFileTransferUpdated` runs on every update. If the state stays `Ready` after a successful save, or the engine re-emits the update synchronously from inside `saveTransferAs`, a second create-only attempt could fail with "exists" and mark a good transfer as errored. I need to see the engine's state transitions after a save.
- **Other writers of `qsFTDownloadDir`:** only lines 4801–4910 were provided. The rest of `MainWindow.cpp` and the config dialog need a search to confirm nothing else writes it from a manual flow.
- **No integration test:** tests cover the helpers, not `MainWindow::onFileCardClicked`. A regression that writes `qsFTDownloadDir` directly wouldn't be caught.
- **`MaxFileNameBytes` isn't visible.** The tests assume 255 (256×`x` and 128×`é` rejected, 255×`x` accepted). The full path length isn't checked (Windows `MAX_PATH`). That only makes saves fail, not unsafe, but it's untested.
- **Missing test rows:**
  - A valid emoji surrogate pair such as `😀.txt`. Only a lone `0xD800` is tested, so the deliberate fail-closed behaviour for supplementary characters isn't pinned.
  - Leading or trailing U+00A0 or U+3000.
  - Leading U+200B or U+FEFF.
  - Lower-case `com¹`/`lpt³`.
  - `name. ` (dot then space).
  - Bare `AUX` or `CON`.
- **`QDir::isAbsolutePath` with drive-relative `C:` or UNC folders** on Windows is untested.

## Residual limits (state these in the work-order closure)

- **Supplementary Unicode:** names with characters outside the Basic Multilingual Plane (emoji, etc.) deliberately fail closed. The check walks UTF-16 units, so each half of a pair counts as a surrogate. Those files can only be saved manually.
- **Not a content filter:** this is not a malware or content filter. Names like `.lnk`, `.desktop`, `desktop.ini`, `autorun.inf` or a spoofed double extension are allowed into the configured folder.
- **Symlinks and shared folders:** a configured folder that is a symlink, or a shared/synced folder, and races on it, are not addressed.
- **Trust freshness:** the trust state comes from `trustStateFor(info.peerSession, …)` at update time. Session reuse and the freshness of the cached or presented key belong to the separate open work order and are not fixed here.
- **No overwrites:** automatic saves still use the existing create-only API and cannot replace existing files.
