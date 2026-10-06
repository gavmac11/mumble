# Video pacing review follow-up

Test date: October 3, 2026, America/Los_Angeles (October 4 UTC in the raw records). This follows the [original October 2 measurements](pilot-pacing-results-2026-10-02.md).

The revised client queue passed both 30-second fixture workloads with **2.5 Mbps per sender and a raised 200 Mbps aggregate video cap**. This tested the default per-sender limit only; it did **not** qualify the fully default 2.5/20 Mbps configuration. Multiple senders can still exhaust the default aggregate cap and lose video without sender feedback. Neither measured workload dropped offered media inside the pacer or after submission to the relay. This is a short regression check of the C++ queue, not a hosting-density or full GUI qualification.

## Review changes

| Comment | Resolution |
| --- | --- |
| 1: Rate exceeds the default budget; hardcoded rates | `VideoQuality::wireBitRate` derives the allowance from the selected profile, the shared 900-byte fragment size, a conservative 1,024-byte protocol-packet bound and 52 bytes of wire overhead. It adds 10% scheduling/burst headroom where possible and caps the result at 2.4 Mbps. Current webcam/screen allowances are 1,972,668 / 2,400,000 bps. An increased profile also clamps the encoder target to fit this same budget. |
| 2: Large IDRs rejected and backlog erased | The front frame can exceed the pending-byte budget and receives its serialization time plus 250 ms of scheduling slack. It is bounded to 4,096 packets, matching the receiver's fragment bound. Pending frames have a one-second wire-byte budget, consistent with the encoder's one-second VBV reservoir. Invalid input leaves accepted work intact; queue overload preserves the front frame and removes dependent pending work. A test drains a 270 KB encoded keyframe completely while rejecting an additional oversized pending frame. |
| 3: No encoder recovery request | `PacketPacer::keyframeRequested` connects to `ScreenCapture::requestKeyframe`. An atomic request is consumed by the next image or YUV encoder submission. The libx264 `forced-idr` option makes that requested I-picture an IDR. After overload, the request is emitted once the retained front frame drains; a scheduling-expired queue requests recovery immediately. |
| 4: Config and evidence disagree | `pilot-server.ini` now contains the historical 3.5/200 Mbps allowances. `pilot-server-defaults.ini` explicitly contains 2.5/20 Mbps. The runbook requires saving the effective runtime configuration. This follow-up used a runtime copy with 2.5/200 Mbps. |
| 5: One packet per timer callback | Each callback drains ready packets using one fixed clock reading. The existing deadline ledger allows at most one additional catch-up packet, so a slow sink cannot create an unbounded callback loop. |
| 6: Rejected traffic counted as relay loss | Probe schema 2 separates offered, accepted and actually submitted fragments. Relay expectations start at submission. Rejections and accepted-but-unsubmitted work have separate counters. The overall `complete_fanout` gate still fails on either client or relay loss. |
| 7: Invisible drops | The pacer exposes a cumulative drop counter and `framesDropped` signal, and logs a warning at most once every five seconds when the count changes. Counts include rejected frames and abandoned queued frames. |
| 8: Redundant teardown | Destruction and `onSelfShareStopped` are the two reset sites. The send callback checks capture and connection state while the queued stop handler is pending. |
| 9: Meta-object and unused API | Added `Q_OBJECT`, tested the meta-object and signals, and removed the test-only `clear` API. The standalone probe bridge now builds the generated meta-object too. |

The protocol does not currently advertise its per-sender video budget. The rate calculation therefore uses the server default; it cannot automatically accommodate an administrator's lower custom cap. Server defaults and the protocol are unchanged. FFmpeg's [libx264 implementation](https://www.ffmpeg.org/doxygen/trunk/libx264_8c_source.html) maps a requested I-picture to an IDR when `forced-idr` is enabled.

## Measurements

Ten authenticated clients used encrypted UDP, ten simultaneous voice senders, 30 seconds of offered media and two seconds to settle. Five-second encoded fixtures repeated six times. The actual C++ `PacketQueue` was driven through the native bridge by Python scheduling. Server and generator each had a one-CPU, 1 GiB container limit on the supplied eight-vCPU VM. A separate client build used up to two other vCPUs during these runs; these are not isolated latency measurements.

Reproduction correction from the next review: this probe version truncated 1.972668 Mbps to 1,972,667 bps when constructing the native webcam pacer, one below the client's 1,972,668 bps value. The current probe rounds instead. The archived measurements and source hashes below have not been rewritten.

| Workload | Video deliveries | Voice deliveries | Voice relay p95 | Scheduled frame to fragment delivery p95 |
| --- | ---: | ---: | ---: | ---: |
| Ten 720p30 webcams | 654,480 / 654,480 | 135,000 / 135,000 | 5.702 ms | 83.673 ms |
| One 1080p15 screen share | 83,916 / 83,916 | 135,000 / 135,000 | 1.411 ms | 372.974 ms |

Raw results: [webcams](results/2026-10-03/pr25-final-default-sender-webcams.json), [screen](results/2026-10-03/pr25-final-default-sender-screen.json), [effective configuration](results/2026-10-03/pr25-final-default-sender-server.ini). Both had zero rejections, zero accepted-but-unsubmitted fragments, zero relay deficits and zero integrity errors. All media used UDP.

Screen pacing now trades additional queueing delay for complete frames within the lower sender allowance: its maximum scheduled-frame-to-fragment delay was 421.126 ms. These timings include generator scheduling and pacing, but exclude capture, encoding and display. They do not establish acceptable interactive screen-sharing latency.

All nine captured webcam streams contained 900 complete frames each; the screen stream contained 450. Their hashes equal the October 2 captures already strictly decoded with FFmpeg 7.1 and verified against repeated source fixtures. [Hash/frame verification](results/2026-10-03/capture-verification.json); no new decode was necessary for those identical bytes.

There were no UDP receive/send-buffer error increases or server CPU-quota throttling during either measured counter interval. The webcam generator used 85.26% of one core during the offer and recorded two throttled periods totaling 1.25 ms over its container lifetime. [Raw counter deltas and scope](results/2026-10-03/counter-summary.json).

## Failed candidate retained

The first review candidate allowed a large front frame but retained a 250 ms pending-byte budget and used only the calculated protocol/wire allowance. It delivered every submitted video fragment and all voice, but failed the overall workload gate because the client queue dropped media: 64,440 webcam deliveries and 4,374 screen deliveries were missing from the offered workload. Those were client deficits, not measured relay loss. [Webcam failure](results/2026-10-03/pr25-default-sender-webcams.json), [screen failure](results/2026-10-03/pr25-default-sender-screen.json), [candidate source hashes](results/2026-10-03/candidate1-source-hashes.json).

The final candidate expands pending capacity to match the encoder's one-second burst reservoir and adds scheduling/burst headroom where the sender ceiling permits. Both factors changed together; these tests do not isolate their individual contribution. The revised accounting keeps the failed candidate visibly failing rather than hiding client drops by shrinking the relay denominator.

## Build, reproduction and limits

The full Linux client built with screen sharing enabled. The packet pacer, video packetizer and video quality profile CTest suites passed, including large-IDR completion, backlog preservation, overload recovery signaling, bounded catch-up, drop counters and teardown. All 22 hosting-tool/native-crypto checks passed. See the [build log](results/2026-10-03/pr25-final-build-20261003.log), [hosting checks](results/2026-10-03/pr25-repair-python-20261003.log) and [tested source/binary hashes](results/2026-10-03/pr25-tested-build-manifest.json). A subsequent whitespace-only formatting pass and rebuild are recorded separately in the [final manifest](results/2026-10-03/final-build-manifest.json).

To reproduce this historical per-sender-only comparison, use the pinned source and [archived effective 2.5/200 Mbps configuration](results/2026-10-03/pr25-final-default-sender-server.ini), rather than the fully default template. Build both native bridges. Use the same encoded fixtures, ten clients, ten voice senders, `--seconds 30 --settle-seconds 2`, and a fresh capture directory. Select ten 720p30 senders with `--video-send-cap-mbps 1.972668`, or one 1080p15 sender with `--video-send-cap-mbps 2.4`; the old probe's rounding caveat above applies. Record UDP and cgroup counters before/after. The test host's previous 3.5/200 Mbps configuration was restored after these comparisons. New default-limit qualification follows the [runbook](host-density-pilot.md) and uses pilot-server-defaults.ini unchanged.

The Qt timer adapter and its recovery signal are unit tested; the network fixtures cannot respond to encoder requests. Full GUI capture, actual forced-IDR recovery and playback still require platform testing, including Windows timer behavior. Longer mixed-media, external-path and physical-host density tests remain the launch gates in the runbook. The larger bounded queue is not a guarantee of low latency under sustained overload.
