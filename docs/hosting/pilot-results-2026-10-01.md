# Preliminary hosting pilot results

Test date: October 1, 2026 in America/Los_Angeles; raw timestamps are October 2 in UTC.

This report preserves the initial results. One screen share plus ten concurrent voice streams passed the short encrypted-UDP application test. Ten encoded webcams plus voice did not: video bursts overflowed the server's UDP receive buffer, and staggering camera starts reduced but did not eliminate missing deliveries. The [October 2 pacing follow-up](pilot-pacing-results-2026-10-02.md) records a client change and successful 30-second tests using the original receive buffer. GUI quality, sustained workloads and sellable host density remain unvalidated.

## Environment and reproducibility

The supplied host is a KVM VM presenting eight vCPUs and about 12 GiB RAM, with existing services. An isolated worktree at commit 1772f827f12018472511e82b20c1cb4ba38108b1 was created without changing its original checkout.

The server was built in Debian 12 with Qt 6.4.2 and GCC 12, using two compiler jobs. The pilot build is Release, SQLite-only, with LTO, Ice, and zeroconf disabled. All submodules were needed, including dependencies referenced only by the license generator. GCC's restrict diagnostic on existing std::string concatenation was retained as a warning instead of an error for this pilot; application source was unchanged.

The server ran as a non-root user in a container capped at one CPU and 1 GiB RAM, with no extra swap allowance. Its filesystem was read-only except for a dedicated runtime directory and temporary files. This is container isolation inside an existing VM, not one newly provisioned KVM guest per customer.

- Debian image digest: sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251
- Pilot runtime image: sha256:172c50e495f90a81aff386b36c494ac02aafb37fc0f70298aed3d2f9cb713b63
- Server binary SHA256: 2c97648941109b142386dc08ccf709abd0808dddc7f9ebb03ee35071252b4154

The [relay probe](../../scripts/hosting/relay_probe.py) opens ten authenticated TLS connections and checks each expected forwarded fragment, including sender identity and payload integrity. Early runs used synthetic payloads. Later runs used an H.264 fixture and captured one observer's reassembled stream for decoding. TLS certificates were verified using a test certificate copied over authenticated SSH.

## Synthetic relay results

Every row offered traffic for five seconds. Sender counts and rates are per community. Percentages describe unique expected fragment deliveries observed by the deadline; they are not measurements of underlying network packet loss. Delay includes client scheduling and the entire test path.

| Path and video limits | Offered workload | Delivery | Relay delay p95 | Raw result |
| --- | --- | ---: | ---: | --- |
| SSH tunnel; 20 Mbps aggregate, 2.5 Mbps/sender | One sender at 2 Mbps, nine viewers | 99.789% | 25.158 ms | [Screen baseline](results/2026-10-01/tcp-default-cap-one-screen.json) |
| SSH tunnel; same limits | Ten senders at 1.5 Mbps | 15.267% | 157.652 ms | [Webcam baseline](results/2026-10-01/tcp-default-cap-ten-webcams.json) |
| SSH tunnel; 200 Mbps aggregate, 2.5 Mbps/sender | One sender at 2 Mbps | 100% | 31.903 ms | [Screen with raised aggregate cap](results/2026-10-01/tcp-200mbps-cap-one-screen.json) |
| SSH tunnel; same raised limits | Ten senders at 1.5 Mbps | 95.487% | 2,142.245 ms | [Webcams with raised aggregate cap](results/2026-10-01/tcp-200mbps-cap-ten-webcams.json) |
| Direct Tailscale; 200 Mbps aggregate, 2.5 Mbps/sender | Ten senders at 1.5 Mbps | 99.257% | 2,168.764 ms | [External direct path](results/2026-10-01/tcp-direct-200mbps-cap-ten-webcams.json) |
| Clients on the supplied host; same raised limits | Ten senders at 1.5 Mbps | 100% | 6.182 ms | [Local path](results/2026-10-01/tcp-local-200mbps-cap-ten-webcams.json) |

The external direct and local runs allowed five seconds to drain after sending; the tunnel runs allowed two. Delivery percentages therefore are not directly comparable as a loss rate.

The local run delivered all 94,500 expected fragments, representing approximately 135 Mbps of outgoing payload during the offered workload. The server consumed 1.067002 CPU-seconds across the complete probe, including connection/setup overhead, and had no throttled cgroup periods. See [before](results/2026-10-01/cpu-local-before.txt) and [after](results/2026-10-01/cpu-local-after.txt) counters. Process high-water RSS was 48,768 KiB when inspected after these runs. This does not include a guest OS or establish a VM memory budget.

During part of the external high-rate test, host-interface transmit readings were approximately 93–100 Mbps while tailnet-interface readings were about 86–92 Mbps. Host aggregate CPU was about 8–15% in those samples. The direct-path probe used about 34% of one client CPU core while offering traffic. [Host counters](results/2026-10-01/host-tcp-comparison.jsonl).

The local success and external delays indicate a bottleneck in the external test path under this workload. They do not identify a contractual 100 Mbps limit or prove which interface, transport, or host layer caused it. A Tailscale connectivity check used a direct LAN path; removing SSH alone did not eliminate the delay.

## Encoded screen-share result

An initial CBR fixture using different encoder settings lost fragments from three of 75 frames; its remaining complete frames decoded successfully. That demonstrates why decoder exit status alone is insufficient. [Initial fixture result](results/2026-10-01/tcp-direct-h264-one-screen.json).

A second fixture matched the client's principal screen-share settings: libx264, superfast preset, zerolatency tuning, 1920×1080, 15 frames sent per second, 2 Mbps target/max rate, a 2 Mb buffer, and a five-frame GOP. The test used synthetic moving imagery, two encoder threads, and access-unit delimiters for extraction; it was not a recording produced by the GUI client.

| Sender limit with 200 Mbps aggregate | Expected fragment deliveries | Observed | Complete captured frames | Result |
| --- | ---: | ---: | ---: | --- |
| 2.5 Mbps | 13,986 | 13,275 | 72 of 75 | [Frames 13–15 incomplete](results/2026-10-01/tcp-direct-h264-client-profile-screen.json) |
| 3.5 Mbps | 13,986 | 13,986 | 75 of 75 | [Complete delivery](results/2026-10-01/tcp-direct-h264-screen-35mbps-sender.json) |

The fixture averaged about 2.18 Mbps over five seconds. A calculation using the server's 360-packet bandwidth window and the fixture's packet sizes reached about 3.03 Mbps, explaining why an average below 2.5 Mbps did not prevent clipping. The observed improvement is consistent with the per-sender guard being the constraint; no per-guard drop counters were added in this experiment.

At 3.5 Mbps, the captured H.264 byte stream matched the fixture's SHA256 exactly. FFmpeg 7.1 decoded all 75 expected frames at 1920×1080 with strict error handling. Relay delay p95 was 36.561 ms on this external TCP/TLS path. [Decode verification](results/2026-10-01/h264-screen-decode.json).

Fixture SHA256: 38bd450ec565dbd6558302fa2750bede1ec0b6c64d40034e24028d53ff0564e8.

Reproduce the fixture with FFmpeg 7.1:

    ffmpeg -n -hide_banner -loglevel error -filter_threads 1 -f lavfi -i testsrc2=size=1920x1080:rate=15 -t 5 -c:v libx264 -preset superfast -tune zerolatency -threads 2 -pix_fmt yuv420p -b:v 2M -maxrate 2M -bufsize 2M -g 5 -bf 0 -x264-params aud=1:repeat-headers=1 -f h264 screen.h264

This is evidence of one short reconstructed stream and full fragment fan-out. It is not a real-time GUI rendering test, voice-quality measurement, or certification of all video workloads. Keep the 3.5 Mbps sender setting provisional while testing webcam bursts and simultaneous streams.

## Consequences for the hosting plan

1. The default 20 Mbps aggregate cap cannot support the intended full ten-webcam workload. A managed configuration needs higher aggregate capacity.
2. Encoder bursts matter as well as average bandwidth. The original 2.5 Mbps sender allowance clipped this representative fixture; a provisional 3.5 Mbps allowance passed this case.
3. Network performance needs direct qualification. A server capable of local fan-out can still give poor external results.
4. Keep host-side quotas because customers have root. An application bandwidth setting is not a customer-enforceable resource limit.
5. Ten encoded webcams plus voice failed despite the synthetic ten-stream workload passing. Protect voice against encoded-video bursts and qualify realistic fixtures before using density results.
6. Do not derive a sellable VM count by dividing CPU capacity by this short single-instance measurement.

Still outstanding: resolving the encoded-webcam burst failure; external encrypted-UDP paths; audible voice and GUI playout; multiple KVM guests; realistic correlated demand; a noisy tenant; long-duration runs; recovery/export; automated lease/expiry; and region-specific network qualification. The [full pilot plan](host-density-pilot.md) remains the launch gate.

## Encrypted UDP and concurrent voice

The [native bridge](../../scripts/hosting/ocb_bridge.cpp) uses the pinned repository's unchanged OCB2 implementation. Each client obtains its key and nonces over verified TLS, establishes encrypted UDP with a ping exchange, and then sends media over UDP. Results count TCP fallback separately. The native checks passed maximum-size packets, 300 packets across nonce wrap, reverse-direction exchange, tamper/replay rejection, and the existing zero-block safeguard. No keys or nonces are included in results.

Clients ran together in a separate one-CPU, 1 GiB container on the same supplied host. They reached the server through its private published TCP/UDP port. These are local-host protocol and burst tests, not measurements of internet latency or customer-VM isolation. Every row offered traffic for five seconds and allowed two seconds to finish receiving. The server retained its provisional 3.5 Mbps sender and 200 Mbps aggregate video limits. Successful rows used UDP exclusively, with no integrity errors or fallback.

| Workload | Video fragments delivered | Voice packets delivered | Video / voice relay p95 | Evidence |
| --- | ---: | ---: | --- | --- |
| One encoded screen, nine viewers | 13,986 / 13,986 | Not offered | 9.664 ms / — | [UDP screen](results/2026-10-01/udp-local-h264-screen.json) |
| Ten synthetic 1.5 Mbps video streams | 94,500 / 94,500 | Not offered | 17.040 ms / — | [UDP synthetic webcams](results/2026-10-01/udp-local-ten-webcams.json) |
| One encoded screen plus ten voice senders | 13,986 / 13,986 | 22,500 / 22,500 | 9.888 / 1.906 ms | [Screen and voice](results/2026-10-01/udp-local-screen-and-voice-noise.json) |
| Ten synchronized encoded webcams plus ten voice senders | 95,292 / 109,080 | 21,933 / 22,500 | 22.902 / 17.375 ms | [First encoded webcam run](results/2026-10-01/udp-local-webcams-and-voice.json) |
| Same synchronized workload, instrumented repeat | 95,382 / 109,080 | 21,969 / 22,500 | 22.785 / 17.679 ms | [Synchronized repeat](results/2026-10-01/udp-local-webcams-voice-sync.json) |
| Encoded webcam starts spread across one frame interval | 105,399 / 109,080 | 22,347 / 22,500 | 26.778 / 22.314 ms | [Staggered comparison](results/2026-10-01/udp-local-webcams-voice-stagger.json) |

The successful UDP screen captures contained all 75 frames and matched the previously decoded source fixture's SHA256 exactly. The webcam fixture used 1280×720 at 30 fps, a 1.5 Mbps target/max rate, 1.5 Mb buffer, superfast/zerolatency, two threads, and a ten-frame GOP. Ten copies deliberately aligned keyframes in the synchronized case. This is a burst scenario, not an estimate of typical customer behavior. Staggering spread the ten senders over 30 ms within a 33.33 ms frame interval; each sender still emitted a whole frame's fragments together.

The voice fixture was five seconds of deterministic pink noise encoded as mono Opus at 48 kHz, 32 kbps, with 20 ms, 80-byte packets. Each sender offered the first 250 packets; the fixture's final container-trimming packet was unused. All received voice payloads in the successful mixed run matched their source packets. This verifies transport integrity and timing, not speech intelligibility, concealment, jitter-buffer behavior, or mouth-to-ear latency. Packet durations are checked against the [Opus TOC specification](https://www.rfc-editor.org/rfc/rfc6716.html#section-3.1).

Reproduce the additional fixtures with FFmpeg 7.1:

    ffmpeg -n -hide_banner -loglevel error -filter_threads 1 -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -c:v libx264 -preset superfast -tune zerolatency -threads 2 -pix_fmt yuv420p -b:v 1500k -maxrate 1500k -bufsize 1500k -g 10 -bf 0 -x264-params aud=1:repeat-headers=1 -f h264 webcam.h264
    ffmpeg -n -hide_banner -loglevel error -f lavfi -i anoisesrc=color=pink:amplitude=0.1:sample_rate=48000:duration=5:seed=42 -ac 1 -c:a libopus -application voip -b:a 32k -vbr off -frame_duration 20 voice-noise.opus

### Burst diagnosis

During the synchronized repeat, server-namespace UDP RcvbufErrors increased by 1,581. Each lost incoming media packet would have been forwarded to nine other clients: 1,581 × 9 = 14,229, exactly the combined missing video and voice deliveries. Neither server nor generator had any CPU quota throttling; the generator host reported no UDP receive-buffer errors. This is strong evidence that the server receive queue caused this run's missing deliveries. [Counter deltas and CPU accounting](results/2026-10-01/udp-burst-diagnosis.json).

The staggered comparison recorded 212 server receive-buffer drops, explaining 1,908 of 3,834 missing deliveries under the same fan-out calculation. The remaining 1,926 are not explained by that counter. Application rate limits and send scheduling need instrumentation before assigning a cause. No client receive-buffer drops or CPU quota throttling were observed in that comparison either. Staggering therefore did not produce a passing workload.

The host's reported receive-buffer default and maximum were both 212,992 bytes. The inspected server socket setup does not request SO_RCVBUF explicitly. No kernel buffer defaults, global network settings, or application source were changed during these initial tests. The subsequent [buffer and pacing comparison](pilot-pacing-results-2026-10-02.md#comparisons-and-unsuccessful-cases) distinguished their effects while checking voice delay.

Raw before/after files are retained beside the [diagnosis](results/2026-10-01/udp-burst-diagnosis.json): server namespace SNMP and cgroup CPU counters, generator cgroup CPU counters, and shared generator-host SNMP counters. The latter can include unrelated host traffic; server counters are confined to the pilot container's network namespace.

### Tone-fixture integrity diagnostic

An earlier 440 Hz tone fixture produced one altered padding byte in Opus packet 10, corresponding to Mumble frame number 20. Across ten senders and nine receivers, this initially appeared as 90 payload-hash failures. The existing OCB2 safeguard deliberately changes the first bit of a zero-filled penultimate AES block; the observed byte and hash match that behavior. A native regression check reproduces the adjustment without changing the cryptographic implementation.

The changed byte was inside Opus padding. Rebuilding the fixture container with that one-byte adjustment and a corrected Ogg checksum produced exactly the same decoded PCM SHA256 as the original under FFmpeg's strict decode. [Decode verification](results/2026-10-01/opus-padding-decode-check.json), [initial mixed result](results/2026-10-01/udp-local-screen-and-voice.json), and [single-sender diagnostic](results/2026-10-01/udp-local-voice-diagnostic.json). The probe continues to report byte differences rather than silently accepting them. The later pink-noise fixture passed byte-integrity checks.

## Current test resources

The dedicated pilot checkout and build/runtime directories remain on the supplied host for continued testing. The build container and original loopback-only server container are stopped. The temporary SSH forwarding session is closed. One limited pilot server remains running, reachable only through the host's Tailscale address on the dedicated test port; its current video limits are 3.5 Mbps per sender and 200 Mbps aggregate. No public hosting offer, purchased server, or production deployment was created.
