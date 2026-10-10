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

## Outstanding qualification

Full [CI 38089837376](https://github.com/gavmac11/mumble/actions/runs/38089837376) and [preview/package 38089839141](https://github.com/gavmac11/mumble/actions/runs/38089839141) were dispatched at source head `0e0671cf5d58cffe905c83e162dd287686a666f2` and remain in progress at this record. Native Windows and isolated Debian server checks are in progress. The security workflow requires a master-targeted PR/push; its updated check remains an integrated-candidate gate.

Global cross-thread media callback accumulation, total server memory across many stalled connections, input buffering, voice/video fairness under saturation, legacy video behavior, transfer tracking bounds, visible budget feedback and hosting profiles remain open. The 16 MiB policy also limits legitimate bursts; qualification of very large initial channel/user synchronization remains open. Local dynamic dependencies require macOS 26 and do not qualify older supported versions. Independent worker shutdown and historical recovery changes must still be integrated and qualified together. Native dialogs/devices, signing, isolation, paid cohorts and measured economics remain release gates.
