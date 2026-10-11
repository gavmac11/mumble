# Fatal startup error regression

The read-only exact Opus 5.5 review identified a configured-database failure with no visible error and unsafe fatal static teardown. The new constructor dialog displays the configured path and SQLite error as plain text, then terminates without opening the fallback database. Fatal logging flushes the diagnostic and exits without static cleanup; normal shutdown retains the existing handler restoration.

## Negative controls

On the unchanged integrated production tree, both new test targets failed. The database tests could not observe a visible configuration error under either Qt’s default handler or Mumble’s production handler. The fatal logger tests observed static cleanup after a Qt fatal message, and a crash during direct fatal logging when cleanup shut down spdlog and then logged through Qt.

The direct logger crash is reproduced. The database Qt-fatal crash is not: Qt’s same-thread message recursion guard prevented that particular cleanup message from reentering Mumble’s handler. Both fatal paths nevertheless ran static cleanup before this repair.

## Validation scope

The database child tests use private settings, private SQLite files and offscreen dialogs. They verify the visible plain-text error, unchanged fallback hash, default Qt fatal exit, and production handler exit status. Logger children verify Qt and direct fatal diagnostics, exit status 1, and that registered unsafe static cleanup never runs. These tests exercise fatal termination; they do not qualify packaged native dialogs on every supported OS.

The repaired client and server built successfully on the current Mac. All 40 enabled CTest suites passed (41 registered; OverlayTest intentionally disabled), with no failures, in 54.84 seconds. The two focused suites contain seven logger and six database cases. The linked full log records the suite result; native Windows and hosted validation of this new repair remain pending.
