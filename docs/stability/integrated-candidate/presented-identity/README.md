# Presented and authenticated file-transfer identity

Source `f337b951482e121964e9162221e96611f94e068e` fixes verification of a cached key instead of the displayed key and blocks a changed saved key when a late join misses the worker cache. Verification includes the expected fingerprint in the database update. A pending prompt binds its server, name, user object, channel, capability and unique GUI token; stale dialog results cannot resolve a newer displayed request.

An additional negative fixture reproduced an unverified sender borrowing a differently keyed, historically verified name for automatic saving. Incoming jobs now retain the handshake name and authenticated fingerprint through Ready and save results. Acceptance and automatic saving check that captured namespace/key. A valid completed sender may rename or disconnect; a missing/replaced/revoked pin prevents Ready. Displayed file attribution uses the captured name when the live name differs.

The full available Mac Release build and all 41 enabled suites pass (42 registered, OverlayTest disabled). The five related suites contain 164 passing Qt cases. This local shared Homebrew build has warnings-as-errors disabled and is not the supported-OS shipping package. Manager fixtures isolate SQLite in memory and explicitly refuse M2 signing; actual crypto/file round trips are separate engine and E2E cases.

Windows/Linux qualification and actual Opus 5.5 review are pending. Installed UI, physical devices, shipping signatures, relay/receipts/backpressure, final recovery and commercial gates remain open. Negative logs retain the precise baseline limitations; no unmodified shipping reproduction is claimed.
