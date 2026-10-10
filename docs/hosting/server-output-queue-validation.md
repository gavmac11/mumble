# Server output queue validation

This is a specific S4 fix, not native media or VPS capacity acceptance.

The source baseline's `Connection::sendMessage()` writes every outgoing frame without bounding retained output. A receiver can remain authenticated while it stops reading file relays. The corrected server enables a **16 MiB per-connection admission budget** covering pending plaintext, pending encrypted output and the next complete frame. The client Connection default remains unlimited. The existing largest permitted protocol frame (8 MiB including its header) fits the server budget.

Both Qt queues must count: [QSslSocket::bytesToWrite()](https://doc.qt.io/qt-6/qsslsocket.html#bytesToWrite) describes pending plaintext; [encryptedBytesToWrite()](https://doc.qt.io/qt-6/qsslsocket.html#encryptedBytesToWrite) describes encrypted output waiting for the network. The comparison subtracts from the budget to avoid integer overflow. TLS record expansion, kernel buffers and allocator retention are outside this admission budget; this is not a hard process RSS bound.

Once a write exceeds the budget, the connection refuses further writes and queues one forced close. Closing synchronously during a server broadcast could remove a user from the channel being iterated. The context-bound queued callback also disappears if the Connection is destroyed first. A full queue closes the session; it does not silently discard individual file/audio/control frames and leave the connection active. The guard applies to all outgoing TCP messages, including tunneled media, though saturation fairness is not qualified by this drill.

## Reproduction and corrected behavior

Production source is `ae03575fb63b08b734ee5ff264e349f946d0fd19`, with the public probe dependency fix at `0e0671cf5d58cffe905c83e162dd287686a666f2`. The baseline is production bandwidth-meter source `7c7d4578e5073d6262e8dea6069c01d4f7ebb128`. The saved baseline server was copied before changing Connection sources. Its later documentation commits do not change the production files.

The [public probe](../../scripts/hosting/test_slow_receiver.py) starts and stops its own loopback server with a new private profile, a freshly generated certificate and hostname/CA verification. Three authenticated peers join one room and announce file capability; an observed capability echo precedes sending. One receiver pauses TLS reads; a healthy receiver checks every payload, index, transfer ID and rewritten actor, while control pings continue. The relay sends 640 × 64 KiB chunks (40 MiB), using the default 16 Mbps sender and 64 Mbps aggregate file limits, over about 28 seconds. These are valid-length opaque relay records; the probe does not substitute for the cryptographic file-engine suite.

| macOS 26.6.2 ARM64 result | Baseline | Corrected |
| --- | ---: | ---: |
| Healthy chunks, exact payload/order | 640/640 | 640/640 |
| Stalled receiver chunks after resuming | 640 | 12 buffered before closure |
| Stalled connection closed | No | Yes |
| Healthy peer observed user removal | No | Yes |
| Healthy control pings answered | 134 | 134 |
| Maximum observed healthy ping | 2.91 ms | 0.92 ms |

RSS samples are preserved in the reports, but these short samples do not establish a process memory ceiling, a latency guarantee or VPS capacity. The default-limit runs had similar peak RSS; heap retention after abort prevents equating RSS directly with the output budget. An initial higher-bandwidth rehearsal also reproduced the stall and corrected disconnect. Only the final public-probe runs appear in the table.

The queue unit test passed **10 QtTest cases** (eight behavior cases plus initialization/cleanup), with zero failures/skips. It checks exact admission, the largest existing frame, disconnected and empty writes, latched rejection before deferred close, the unlimited client default, a maximum-width backlog and destruction before the callback. Its controllable socket exercises production Connection code without depending on kernel buffer timing; the relay drill exercises real TLS/server connections. Six targeted CTest suites passed in 7.44 seconds: Connection output queue, bandwidth meter, actual encrypted file-transfer engines over the server, cryptography, protocol encoding and audio receiver routing.

See the [validation manifest](results/2026-10-10/output-queue/validation.json), [baseline relay](results/2026-10-10/output-queue/mac-baseline.json), [corrected relay](results/2026-10-10/output-queue/mac-fixed.json), [unit report](results/2026-10-10/output-queue/mac-unit.txt) and [targeted CTest report](results/2026-10-10/output-queue/mac-regression.txt).

## Native Windows and Debian server

Native Windows 10 Pro 19045 x64 tested exact source `0e0671cf5d58cffe905c83e162dd287686a666f2`. MSVC 19.51.36257.0 / Qt 6.10.0 static built the server and tests in Release with `/W4 /WX` and no source warning relaxation. The output queue test passes all 10 cases and the actual encrypted file-transfer E2E passes all five cases, without failures/skips. CTest took 10.09 seconds. These are native automated checks, not a Windows bulk paused-reader, memory, sanitizer, GUI or Windows 11 qualification. The [Windows report](results/2026-10-10/windows-output-queue/validation.txt) and all 11 producer report checksums match committed Git bytes; report newline conversion is disabled explicitly.

The same source and pinned submodule commits built the actual server in an isolated Debian 12 amd64 container on the Ubuntu lab host, using Qt 6.4.2, OpenSSL 3.0.22 and protobuf 3.21.12. The existing pilot GCC 12 `-Wno-error=restrict` exception for unchanged DataType.cpp remains; this is not an all-warnings-fatal GCC qualification. The container has two CPUs, 3 GiB, no external network, a read-only root, UID 1000 and dropped capabilities. The queue and bandwidth CTest suites pass. The [real TLS relay](results/2026-10-10/output-queue/debian-relay.json) delivers all 640 exact chunks and answers 134 healthy pings, with the stalled peer closed and its removal observed. Its 77 received chunks after resuming were buffered before the forced close. Maximum observed healthy ping was 0.55 ms in this single local run. These limits do not measure tenant isolation, Ubuntu desktop behavior or physical-host density. Initial source exports missed dependency files used by configuration/license generation; those incomplete attempts are not qualified. The final export includes every pinned submodule.

## Outstanding qualification

Full [CI 38089837376](https://github.com/gavmac11/mumble/actions/runs/38089837376) and [preview/package 38089839141](https://github.com/gavmac11/mumble/actions/runs/38089839141) were dispatched at source head `0e0671cf5d58cffe905c83e162dd287686a666f2`. Preview/package gates passed Windows, Ubuntu, ARM macOS and Debian; publication was skipped. Full CI passed Ubuntu and both Mac architectures, with Windows still running at this record. Native Windows and isolated Debian server checks passed (details below). The security workflow requires a master-targeted PR/push; its updated check remains an integrated-candidate gate.

Global cross-thread media callback accumulation, total server memory across many stalled connections, input buffering, voice/video fairness under saturation, legacy video behavior, transfer tracking bounds, visible budget feedback and hosting profiles remain open. The 16 MiB policy also limits legitimate bursts; qualification of very large initial channel/user synchronization remains open. Local dynamic dependencies require macOS 26 and do not qualify older supported versions. Independent worker shutdown and historical recovery changes must still be integrated and qualified together. Native dialogs/devices, signing, isolation, paid cohorts and measured economics remain release gates.

Native Mac UI checking after unlock found a reproducible Qt accessibility crash in both the developer Qt 6.11.1 runtime and the packaged Qt 6.10.0 preview. A private experimental Qt patch permits limited connection and identity decline/retry checks, but the shipping dependency fix remains a release blocker. See the [Mac evidence](../stability/mac-native-preparation-20261010.json).
