# File-transfer client integration test

This target links the production client library and bundled dark theme. Its
document test creates the actual MainWindow, manager and log, and checks outgoing
and incoming card updates under a child-process watchdog.

When the server target is enabled, it also starts an owned loopback server on an
available port and two separate client processes. They use production
ServerHandler TLS connections, MainWindow protocol handlers, FileTransferManager,
the worker engine and the real send and safety-number dialogs. A test-only timer
selects the other generated fixture client and clicks the dialog buttons.
Preverified, first-contact (opt-in safety-number dialogs) and first-use (default
trust on first use, no dialog allowed) transfers must save the 600123-byte input
unchanged and acknowledge delivery. The first-use row also checks that both sides
pinned the other's key as not verified. During transfer, the GUI timer must keep running with
no observed gap above two seconds.

All databases, TLS pins, identities, source files and saved files are disposable.
The fixture does not load personal settings, discover personal themes, initialize
hardware plugins, start audio, install native shortcut hooks or activate overlay
helpers. The plain file-transfer rows supply the selected file path directly to
MainWindow::startFileSend; the native file picker itself is not covered.

The `inlineWebMDrop` rows deliver synthetic Qt drag-enter/drop events containing a
local file URL to the real chat viewport. A generated silent VP9 WebM fixture
must reach both chat documents unchanged, decode its poster and advance playback
frames when playback is compiled in. The oversized row negotiates an image limit
below the fixture size, then exercises the normal file-send and first-contact
dialogs and verifies the saved WebM hash. These tests cover the Qt drop handler,
not an Explorer/OLE drag performed with physical input.

Build and run through CTest:

```text
cmake --build <build> --target TestFileTransferMainWindow
ctest --test-dir <build> -R ^TestFileTransferMainWindow$ --output-on-failure
```

CTest allows 900 seconds for all rows. Each of the four TLS rows has bounded
process startup (up to 15 seconds total), server readiness (60 seconds), TLS probe
(15 seconds), client execution (75 seconds), and cleanup (up to 28 seconds including
a repeated kill wait on failure). The
document row allows another 30 seconds; the remaining budget covers fixture
preparation and scheduling. Server readiness reuses one TCP socket across probes.

CTest uses the offscreen Qt platform. For the native Windows Qt platform, run
from the build directory with the same runtime DLL search path used by CTest:

```text
.\tests\TestFileTransferMainWindow.exe -platform windows -o mainwindow-windows.txt,txt
```

Windows GUI executables require the explicit QtTest output file to retain the
report reliably. Children inherit the parent's Qt platform. Set
`MUMBLE_TEST_FT_CONTACT_DELAY_MS=15000` to leave each safety-number dialog open for
15 seconds before the test accepts it; the supported range is zero to 20000 ms.

Full child/server diagnostics are saved under
`test-results/file-transfer-main-window` in the working directory. Each row
overwrites its prior logs, so preserve that directory between diagnostic runs.
Child logs include state transitions, the maximum GUI heartbeat gap and teardown
checkpoints. Failures report exit codes and output tails even if QtTest truncates
the full diagnostic text.
