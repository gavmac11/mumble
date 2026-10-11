# Audio wizard unavailable devices

The available Mac reproduced SIGSEGV in `QThread::start` from `AudioWizard::on_qcbInputDevice_activated`: CoreAudio returned no input while microphone permission was pending, and the wizard dereferenced it. The same unchecked output path also existed.

The wizard now starts only created inputs/outputs, handles missing backend selections, and keeps device setup incomplete while either side is unavailable. A visible plain-text message explains microphone access or output selection. The existing ticker refreshes this state if permission completion later creates the input.

All four focused cases pass, covering unavailable input with and without output, output recovery while input remains unavailable, and removed backend selections. Tests use private configuration and fake output without hardware capture. Full Release client/server build and all 41 enabled suites pass in 48.47 seconds. Early fixture initialization/thread teardown errors were corrected and excluded.

The ad hoc signed native developer bundle then opened its owned SQLite database, entered the wizard and selected MacBook Pro Microphone without crashing. Unavailable-input guidance remained visible and Next stayed disabled. Cancel and normal Quit succeeded; no new client crash report followed that accepted retry. A stale-signature launch rejection before the accepted retry is a fixture limitation, not application startup acceptance. The private Cocoa repair remains in use.

This qualifies crash prevention while permission is pending. Microphone capture, actual audible loopback, permission approval, camera/screen, supported OS installations and shipping signatures remain open.

Exact source `8c4269054798f6e3104426f1171dd77ef0a06465` also passes strict native Windows Release client/server builds and all four TestAudioWizard cases without failures/errors/skips. The [native Windows record](../../../hosting/results/2026-10-10/windows-audio-wizard/validation.json) retains 12 verified committed byte checksums. This is automated device-unavailable handling, not physical microphone/audio or installed-GUI qualification. Linux qualification remains pending.
