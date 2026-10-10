# Server bandwidth meter validation

The shared voice/video/file bandwidth meter stored each frame's charge as an unsigned 16-bit value. File data can carry a 1 MiB chunk plus an authentication tag, and aggregate relay charges can be larger still. When a ring entry expired, only the truncated charge was subtracted, so the rolling total drifted upward during a long transfer. The sum itself was a signed int, and admission arithmetic used `long` for elapsed microseconds and the scaled byte total; those quantities exceed a 32-bit Windows long.

The corrected meter stores full int-width charges and a 64-bit rolling sum, elapsed duration and scaled rate. A compile-time bound proves that the maximum 360-entry scaled sum fits in 64 bits. Invalid/nonpositive charges or limits are refused, and the existing int-valued statistics API clamps large positive results. The 360-entry window and configured rates keep their existing policy. The component is separated from `ServerUser` so the production implementation can run in a standalone Qt test on every client/server CI configuration.

On Apple Silicon macOS 26.6.2, the original methods were extracted byte-for-byte from foundation `59eb5be1fc116630afd1bdd4501d07dc8cd60bdf`. Four of eight behavior checks failed (10 QtTest cases including initialization/cleanup):

- A 1 MiB + 34-byte charge was not fully retired after the 360-entry ring wrapped. The rolling sum was 378,548,176 instead of 377,499,600 bytes.
- Two maximum-width charges wrapped the signed sum to −2 instead of 4,294,967,294.
- Large recent charges produced truncated statistics instead of a clamped positive result.
- An invalid negative charge was accepted.

A sanitizer build of the original implementation aborted with an explicit signed overflow at `2147483647 + 2147483647`. The corrected implementation passes all eight behavior checks (10 cases) under address/undefined-behavior sanitizers and in the regular release build, with no failures/skips. Qt/Homebrew dependencies were not sanitizer-instrumented, and leak detection was disabled; this is not whole-application sanitizer qualification.

The corrected server and affected test targets built. Five targeted CTest suites passed in 8.42 seconds: the bandwidth meter, actual TLS/server file-transfer relay (plain/password/capability paths), protocol encoding/decoding, cryptography and audio receiver routing. These checks do not measure audible quality, native dialogs, slow TCP receivers, long mixed sessions or production capacity. Local dynamic dependencies require macOS 26 and do not qualify older supported releases.

Tests also cover the 3000-byte scaled-rate boundary and a forty-minute idle slot that exceed a Windows long, small voice-sized ring behavior, rejected-frame state preservation and maximum-width ring retirement. Native Windows execution and refreshed cross-platform/package CI remain pending for this change; passing those boundary cases on LP64 macOS is not Windows runtime evidence.

See the [validation manifest](results/2026-10-10/bandwidth-meter-validation.json), [original failures](results/2026-10-10/bandwidth-baseline-test.txt), [original sanitizer overflow](results/2026-10-10/bandwidth-baseline-sanitizer.txt), [corrected sanitizer tests](results/2026-10-10/bandwidth-fixed-sanitizer.txt) and [targeted server checks](results/2026-10-10/bandwidth-regression-ctest.txt). This completes a specific accounting fix within S4; visible budget feedback, tested hosting profiles, TCP backpressure and mixed-client behavior remain open.
