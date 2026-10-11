# Trust lifecycle follow-up

PR51 source `9b5b8e26684d6f2481591b11653397d390d39300` qualifies specific identity/sender-snapshot corrections, with exact [review and native evidence](presented-identity/revision-9b5b8e/README.md). The remaining Should items are required before release acceptance, not silently waived by a green automated suite.

| Priority | Work | Required evidence |
| --- | --- | --- |
| First | First-use sender authorization | Preserve the selected name, key/trust and user/context through M4. A rename cannot create a pin under another name. Separate send and receive prompts by transfer/kind/token. Decline must cancel the intended send without resolving an unrelated receive handshake; verify the intended content-release behavior and document it. Test a newly appearing changed pin before M4 and actual Manager::startSend. |
| First | Receive context and cancellation | Obtain receiving name/key atomically. Tag inbound controls/chunks with their original connection; stale queued messages cannot acquire a new connection generation. Cancel stale dialogs by the actual worker request token, including same-key replacements, without dropping a newer prompt or stalling retries for 60 s. |
| First | Manual save and terminal errors | Recheck the captured namespace/key immediately before save. A revoked/changed pin prevents publication. Differentiate missing identity/storage errors from changed-key warnings; a later abort cannot erase the useful failure reason. Test through the production manager with real crypto and actual owned output files. |
| Next | Re-verification and storage failures | Explicit key replacement/removal matches the expected fingerprint, checks the affected row count and refreshes caches. Receive-side storage errors fail distinctly rather than becoming first contact. Re-verification UX and namespace naming/case rules require installed-client and server-rule qualification. |
| Next | Disabled feature and GUI integration | Avoid repeated connection invalidation on every user event while disabled. Exercise real user-event cache refreshes, send/verification dialogs, disconnect/session reuse and file actions with the installed client. |

The manager decision fixtures now count attempted signing, so resume(true) and resume(false) differ. Real engine/E2E crypto cases still bypass the production manager/GUI; that acceptance gap remains. The local Mac controller is unavailable despite unlock replies; Windows capture/focus and physical Ubuntu devices remain unavailable. No unavailable hardware or shipping/media gate is marked passed.
