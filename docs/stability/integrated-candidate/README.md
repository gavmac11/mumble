# Integrated stabilization candidate

This candidate combines the client foundation, cooperative worker shutdown/unsaved receive cleanup, pinned deployment/current/historical recovery tools, full-width bandwidth accounting, stalled TCP output bounds, Qt Cocoa ownership/logging repairs, full binary safety QR verification, keyboard file actions and explicit client database isolation. Each earlier evidence record identifies its own source/package and remains limited to that source.

The code tree at `740649f89` fully builds on the available Mac. All [40 enabled CTest suites](local-regression.txt) pass in 46.39 seconds (41 registered; OverlayTest disabled), without execution failures/skips. All [59 hosting Python checks](hosting-regression.txt) pass without skips when the native crypto/pacer bridges are freshly compiled and configured. The first broad invocation lacked those bridge paths and skipped five cases; it does not qualify the full suite. Private bridge builds use current production sources, and the pacer bridge retains strict warnings.

[Validation](validation.json) separates this integrated local result from earlier native Windows/packaged Mac/guest rehearsal results. Later commits containing only retained evidence do not establish a new binary qualification. Hosted CI, package gates and security analysis must qualify their exact candidate or PR-merge revision.

PR29 remains open with human review required; its transport source is an ancestor of this candidate. This draft does not approve or merge any dependent change. Physical media/permission flows, full accessibility behavior, native Windows 11/Mac15/27/Ubuntu desktop coverage, signatures, endurance, new-candidate deployment/recovery, provider policies and density, and paid-cohort economics remain open.
