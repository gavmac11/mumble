# Upstream sync compatibility evidence — 2026-10-21

**COMPAT BREAK:** the official stable server cannot directly open the fork's schema-11 database. This behavior predates this merge; official master produces the same failure when its database is opened by stable. The protocol and all four native client/server interop checks pass.

## Revisions and merge resolution

- Fork master: `6c22acff6b1ef3fc9a776dd81ccd02108d5804b6`; upstream master: `c7a8993caa4b657c93d3c2456e6cf2eaaafe9e85`.
- Initial ancestry count: **129 ahead / 248 behind**. PR #53 was already merged; no open sync PR or competing active sync run existed.
- Official stable: `v1.5.939`, published 2026-10-11, peeled commit `dfbac8d3d9c967003e74b4ffc5610f7d45e8f666`. The final reference build sets `BUILD_NUMBER=939` and advertises 1.5.939.
- Merge `aee5d253e` has parents fork master and current upstream master. It records the previously missing upstream ancestry without rebasing or squashing.
- PR #53's reviewed head `20ca14ca4` and fork master have the identical tree `049b4324efead57c53b7a3a41c91344062f76d8e`. #53 integrated upstream `9ef78aac077e3e1555b0aecf640c941ef1f093f5` but was squash-merged, so an ordinary merge rediscovered its old conflicts. The content merge uses that recorded upstream revision as the explicit base (`git merge-tree --write-tree --merge-base=9ef78aac... HEAD upstream/master`), preserving all reviewed resolutions and fixes. The resulting tree was installed into the pending two-parent merge.
- The newly merged upstream range is `9ef78aac0..c7a8993ca` (two commits, including the upstream merge commit). The ancestry range is `1ce8d4262..c7a8993ca` (248 commits already largely integrated by #53).
- The upstream-only content change removes five lines for an unused `unixMurmur` global/assignment in `src/murmur/main.cpp`. There are no new content-conflict resolutions. AGENTS.md, contributor policy, AI tooling and all other fork content remain intact.

## Compatibility fixes

The audit found four existing required extension fields. FileTransferControl.payload and FileData.transfer_id/chunk_index/data are now optional on the wire, retaining their original tags and types. Handlers preserve semantic validation, including distinguishing a missing chunk index from explicit zero. Existing well-formed fork messages retain the same encoding.

ServerConfig.video_relay_version (optional field 12) explicitly enables video sending. Video and file sending start disabled, reset on reconnect/disconnect, and enable only with server support. File-transfer identity setup waits for relay negotiation and respects the file-sharing setting. Official servers therefore do not trigger setup prompts or enable unsupported controls. Partial config messages retain negotiated support. Older fork servers need to advertise video relay support before the new client enables sending.

The descriptor audit preserves every official field number/type/label/default and enum value. Fork TCP types 27/28 and protobuf UDP Video=2 do not collide with official IDs. PROTOCOL.md is an additional fork document; it documents the optional fields and required handler validation. Standard version encoding and the 1.5 protobuf/legacy UDP boundary are unchanged. Pre-1.5 clients remain excluded from video relay.

## Compatibility matrix

| Check | Result | Evidence |
|---|---|---|
| Protocol against official master | PASS | [descriptor/ID comparison](protocol-master.json) |
| Protocol against official stable | PASS | [descriptor/ID comparison](protocol-stable.json) |
| Audit detects existing required fields | PASS: four baseline failures detected | [baseline audit](protocol-baseline.json) |
| Version/capability negotiation | PASS: official servers leave both controls disabled; no identity prompt | [master](interop/master-server/result.json), [stable](interop/stable-server/result.json) |
| Official master client → fork server | PASS, 125.43 s | [result](interop/master-client/result.json), [screenshot](interop/master-client/soak-complete.png) |
| Fork client → official master server | PASS, 125.33 s | [result](interop/master-server/result.json), [screenshot](interop/master-server/soak-complete.png) |
| Official v1.5.939 client → fork server | PASS, 125.08 s | [result](interop/stable-client/result.json), [screenshot](interop/stable-client/soak-complete.png) |
| Fork client → official v1.5.939 server | PASS, 125.35 s | [result](interop/stable-server/result.json), [screenshot](interop/stable-server/soak-complete.png) |
| Fork-to-fork capability controls | PASS: video/file controls enabled | [result](interop/fork-positive/result.json), [screenshot](interop/fork-positive/connected.png) |
| Official master DB/config → fork | PASS: authentication, saved channel, integrity | [result](database/master-to-fork/result.json) |
| Fork DB/config → official master | PASS: authentication, saved channel, integrity | [result](database/fork-to-master/result.json) |
| Official v1.5.939 DB/config → fork | PASS: migration, authentication, saved channel, integrity | [result](database/stable-to-fork/result.json) |
| Fork DB/config → official v1.5.939 | **FAIL**: schema 11 cannot be read by schema 9 | [result](database/fork-to-stable/result.json), [server log](database/fork-to-stable/server.log.gz) |
| Official master DB → official v1.5.939 (control) | **FAIL**, same SQL metadata error | [result](database/master-to-stable/result.json), [server log](database/master-to-stable/server.log.gz) |

Each native client joined a permanent channel, displayed both users, sent and received text, was moved back to Root and rejoined Interop. A final text message after the soak verified continued GUI processing. Clients were muted/deafened for the headless environment. Result files record intentional SIGTERM cleanup separately from liveness during the measured session. Compressed client/server logs and observer frame records sit beside each result. Screenshots were inspected. Only disposable loopback credentials were used; published server logs omit the fixture password.

## Build and tests

CMake 3.28.3, Ninja, GCC 13.3, Ubuntu 24.04, Qt 6.4.2 for fork/master and Qt 5.15.13 for stable. Fork client/server, screen sharing, file sharing, chat WebM, plugins and tests built in Debug without unity or LTO. Project warnings-as-errors were enabled. The 12 conversion warnings in unmodified positional-audio plugins are baseline warnings; no new compiler warnings were introduced. See [warning inventory](baseline-build-warnings.txt), [final build log](build-final.log.gz), [freshness check](build-freshness.log.gz), and [configuration](configure.log.gz).

```sh
cmake -S <fresh-worktree> -B ~/.cache/mumble-sync-build/2026-10-21 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/mumble_dependency_provider.cmake \
  -DBoost_NO_BOOST_CMAKE=ON -DCMAKE_UNITY_BUILD=OFF \
  -Dclient=ON -Dserver=ON -Dtests=ON -Dwarnings-as-errors=ON \
  -Ddatabase-sqlite-tests=ON -Ddatabase-mysql-tests=OFF \
  -Ddatabase-postgresql-tests=OFF -Doverlay-xcompile=OFF -Dlto=OFF
cmake --build ~/.cache/mumble-sync-build/2026-10-21 -j 2
QT_QPA_PLATFORM=offscreen ctest --test-dir ~/.cache/mumble-sync-build/2026-10-21 \
  --output-on-failure --output-junit <evidence>/ctest.xml
```

Full CTest: **41 passed, 2 failed, 1 previously disabled**, 403.54 seconds. [CTest log](ctest.log), [JUnit XML](ctest.xml). The file-transfer E2E suite passed in 275.16 s; the manager suite passed in 59.93 s. The protocol suite and missing-chunk-index relay regression pass ([protocol log](protocol-regressions.log), [relay log](chunk-index-regression.log)).

The two failures reproduce in copied pre-sync Debug test binaries from the earlier reviewed worktree, run outside that worktree: TestChatWebM has the same four audio-clock/timing failures ([baseline log](baseline-chat-webm.log)); TestFileTransferEngine aborts in selectedPinSurvivesCacheRemoval on QList::first() of an empty list ([baseline log](baseline-engine.log)). Their affected production/test sources are unchanged from origin/master. This is focused baseline verification, not a rebuilt complete baseline suite. No failing tests or assertions were disabled.

Both reference source checkouts are clean and cached by full SHA under `~/.cache/mumble-upstream-ref/`. They build client/server in Debug with plugins/tests off, unity/LTO/overlay cross-compilation off. Master uses the dependency provider; stable uses Qt 5 and `BUILD_NUMBER=939`. [Build manifest and binary hashes](build-manifest.json), [master build](ref-master-build.log.gz), [stable release build](ref-stable-release-build.log.gz).

## Failure cause and proposed remedy

Direct database downgrade is an existing fork compatibility limitation inherited from the earlier upstream database migration, not this merge's unused-variable removal. The fork's database schema/migration sources are unchanged in this PR. Stable expects schema 9 metadata columns `keystring`/`value`; schema 11 uses a different layout, producing `no such column: value`. Official master reproduces it independently.

For rollback, restore a consistent pre-upgrade schema-9 snapshot with its matching stable binary/configuration/TLS state, as described in the existing [rollback rehearsal](../../../hosting/historical-schema-rollback-rehearsal.md). Supporting current schema-11 state in stable would require a separately reviewed export/down-migration that verifies authentication, channels, groups, ACLs, bans and TLS identity. Do not merely relabel schema metadata. No automatic downgrade was added.

An exploratory fixture also caught a SIGTERM shutdown hang after a failed harness assertion: synchronous signal cleanup re-entered GLib while its socket-notifier lock was held ([stack](shutdown-hang.bt)). The relevant cleanup/stopThread code is unchanged; this was not reproduced in the final passing fixtures and is not claimed fixed. The harness records shutdown timeouts and allows disconnect processing to settle before cleanup.

Windows/macOS builds, MySQL/PostgreSQL service tests, online tests, physical media capture and audio quality were not exercised. OverlayTest was already disabled. This evidence establishes the listed protocol, GUI/control and SQLite checks, not broader platform or media-quality coverage. The PR is not merged.
