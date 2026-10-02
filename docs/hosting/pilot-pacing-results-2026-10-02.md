# Video pacing pilot results

Test date: October 2, 2026. This continues the [initial application pilot](pilot-results-2026-10-01.md).

Client packet pacing passed two short application workloads that matter for hosting: ten encoded webcams with ten voice senders, and one encoded screen share with ten voice senders. Every expected delivery arrived during each 30-second test. The original server receive buffer was sufficient in these runs. This resolves the reproduced burst case for these fixtures; it does not establish production quality or customers per physical host.

## Measured results

Each run had ten authenticated clients, encrypted UDP media, 30 seconds of offered traffic and two seconds to settle. Clients and the server ran on the supplied VM, in separate containers limited to one CPU and 1 GiB RAM each. The server retained the pilot allowances of 3.5 Mbps per video sender and 200 Mbps aggregate. Its binary was unchanged.

| Workload | Video fragment deliveries | Voice packet deliveries | Voice relay p95 | Video relay p95 | Scheduled frame to fragment delivery p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ten 720p webcams at 30 fps | 654,480 / 654,480 | 135,000 / 135,000 | 3.596 ms | 3.041 ms | 75.845 ms |
| One 1080p screen share at 15 fps | 83,916 / 83,916 | 135,000 / 135,000 | 1.252 ms | 0.965 ms | 105.948 ms |

Raw results: [webcams](results/2026-10-02/udp-default-native-paced2-30s-sync.json) and [screen share](results/2026-10-02/udp-default-native-screen3-30s-sync.json). Both reported zero integrity errors and zero media deliveries through TCP fallback. Voice bypassed the video queue. Relay delay starts at actual packet submission; scheduled-frame delay also includes waiting in the pacer. Neither is mouth-to-ear or display latency.

The webcam fixture averaged approximately 1.64 Mbps of video payload per sender, above its 1.5 Mbps encoder target. Its ten streams therefore represented about 147.54 Mbps of outgoing video payload after nine-way replication. The screen fixture averaged 2.18 Mbps, about 19.64 Mbps after replication. Protocol overhead and voice are additional. Targets must not be treated as measured traffic ceilings.

All nine webcam streams captured by one observer contained 900 complete frames each and had identical hashes. The screen capture contained all 450 frames. One capture from each workload decoded without errors using FFmpeg 7.1 with strict error handling. Each capture also matched six repetitions of its source fixture byte for byte. [Decode and hash verification](results/2026-10-02/native-pacer-video-decode.json).

Server and client UDP receive/send-buffer error counters did not increase, and neither container recorded CPU quota throttling. The server used 18.882266 CPU-seconds in the webcam counter interval and 4.708507 in the screen interval, including connection/setup and settle time. The webcam generator used 82.62% of one core during the offer; this generator would need more capacity or distribution before scaling the workload. [Counter summary](results/2026-10-02/pacing-counter-summary.json).

## What changed in the client

The old send loop submitted every fragment of an encoded frame immediately. The [new packet pacer](../../src/mumble/VideoPacketPacer.cpp) spreads those submissions over time. [MainWindow](../../src/mumble/MainWindow.cpp) selects a 2 Mbps pacing allowance for webcams and 3 Mbps for screen sharing; these include a conservative 52-byte per-packet allowance for IPv6, UDP and encryption overhead. Encoder targets remain unchanged.

The queue accepts up to 250 milliseconds of wire-accounted data and discards stale or overloaded video until an independent keyframe can restart decoding. It preserves the next send deadline when replacing a backlog, and idle time does not accumulate burst credit. At the selected rates, queued wire-accounted bytes are bounded to 62,500 for webcams and 93,750 for screens; container bookkeeping adds memory beyond those byte counts. An event-loop delay permits at most one extra catch-up packet.

Stopping capture or disconnecting destroys the pacer and discards queued video. Its destination is tied to the original server connection. Voice continues through the existing send path. The patch was tested before being committed for review; it has not been released or installed for customers.

The network probe calls the actual C++ PacketQueue through a [native bridge](../../scripts/hosting/pacer_bridge.cpp), with a Python event loop driving deadlines. The Qt timer adapter has a separate unit test. These runs do not exercise the complete GUI capture, send and playback pipeline.

## Comparisons and unsuccessful cases

Increasing the server socket buffer alone did not produce a passing ten-webcam workload. A controlled five-second run requested a 1 MiB buffer and observed 2 MiB effective SO_RCVBUF accounting, while preserving IP_PKTINFO. All 22,500 voice deliveries arrived, but only 103,050 of 109,080 video deliveries did. Voice relay p95 rose to 45.994 ms. Kernel receive-buffer errors were zero, so the remaining 6,030 missing video deliveries cannot be attributed to that counter. Application limiting or another relay-path cause remains uninstrumented. [Buffer comparison](results/2026-10-02/udp-buffer1m-v2-sync.json), [socket metadata](results/2026-10-02/buffer-experiment-socket.json).

Earlier buffer experiments did not preserve IP_PKTINFO and are retained as superseded calibration data. The corrected comparison above is the buffer-only result used here. The launcher and preload shim are isolated experiment tools. No host sysctls changed; the unchanged server binary ran without capabilities after socket preparation. The experimental servers are stopped and the original limited pilot server is restored.

A 2.5 Mbps screen pacing allowance also failed: the bounded queue abandoned backlog, leaving three of 75 captured frames incomplete and 459 expected deliveries missing. Its enqueue-rejection count was zero because an accepted replacement keyframe can discard queued work. Delivery and frame counts remain the acceptance criteria. [Screen at 2.5 Mbps](results/2026-10-02/udp-default-native-screen-sync.json).

Raising only the screen pacing allowance to 3 Mbps passed the [five-second comparison](results/2026-10-02/udp-default-native-screen3-sync.json) and the 30-second run above. This is the current client setting. The server's ordinary 2.5 Mbps sender and 20 Mbps aggregate defaults have not changed; the hosted configuration needs the tested allowances and further qualification.

## Build and reproduction

The full Linux client built successfully with screen sharing enabled. The packetizer, packet pacer and video quality profile test suites all passed. The pacer suite covers ordering, wire accounting, bounded backlog, keyframe recovery, stale partial frames, idle and stalled scheduling, and cancellation of pending timer sends. All 19 hosting-tool and native-crypto checks also passed. [Client build and CTest log](results/2026-10-02/client-build-final-rate3.log), [final cancellation-test check](results/2026-10-02/client-pacer-final-check.log), [hosting checks](results/2026-10-02/hosting-tests-20261002.log).

The [build manifest](results/2026-10-02/pacing-build-manifest.json) records the base commit, hashes of the modified source, exact binaries, build image and validation scope. The client build uses the [disposable-container script](../../scripts/hosting/build_client_pilot.sh), with two compiler jobs and a 3 GiB memory cap. Host packages and unrelated services were left unchanged. Windows and macOS builds have not been checked for this patch.

Use the fixture commands and private-server setup in the [pilot runbook](host-density-pilot.md). Build the [crypto bridge](../../scripts/hosting/build_crypto_bridge.sh) and [pacer bridge](../../scripts/hosting/build_pacer_bridge.sh) in a designated disposable build container. Set PILOT_PRIVATE_ADDRESS to the reachable private address of your test server. With fixtures, the public test certificate and bridge libraries mounted, the webcam invocation is:

```sh
python3 /source/scripts/hosting/relay_probe.py \
  --host "$PILOT_PRIVATE_ADDRESS" --port 64759 --server-name localhost --ca-file /pilot.crt \
  --transport udp --crypto-library /build/libmumble_pilot_ocb.so \
  --clients 10 --senders 10 --fps 30 --width 1280 --height 720 \
  --h264-fixture /fixtures/webcam.h264 --video-send-cap-mbps 2 \
  --native-pacer-library /build/libmumble_pilot_pacer.so \
  --voice-senders 10 --opus-fixture /fixtures/voice-noise.opus \
  --seconds 30 --settle-seconds 2 --capture-directory /results/new-webcam-capture
```

For the screen case, change video senders to 1, fps to 15, dimensions to 1920×1080, fixture to screen.h264, pacing allowance to 3, and select a fresh capture directory. The probe records source and library hashes in each result. Capture before/after server namespace and client host UDP counters and both cgroups' CPU counters, as in the retained raw files.

## Remaining launch gates

1. Exercise the compiled GUI client, including actual camera/screen capture, playback, stopping, reconnecting and TCP fallback. Check the supported desktop platforms and measure audible voice quality and displayed video delay.
2. Test one screen plus nine webcams together, realistic speech and varying encoded content, an external UDP path, then the longer mixed-media and soak stages in the runbook. Thirty seconds of repeated synthetic fixtures is not sustained-service validation.
3. Use a bare-metal or Proxmox host for multiple independent customer VMs, correlated demand and a noisy tenant. The supplied eight-vCPU machine is itself a VM; it cannot establish physical-host overselling capacity.
4. Validate shared-IPv4 port allocation, root isolation, automated creation/expiry, portable state export and restore, and seven-day post-expiry recovery. Use the measured capacity and complete provider quote to set rental prices and margins.

No sellable VM density, overselling ratio, production quality guarantee, recovery guarantee or final supplier choice follows from these tests.
