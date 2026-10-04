# Default video limit comparison

Test date: October 4, 2026. Two webcam senders in a ten-user channel failed the fully default **2.5 Mbps sender / 20 Mbps aggregate** video-limit test: 45,216 of 130,896 submitted video deliveries were missing. All voice arrived. The same workload passed with the raised **3.5/200 Mbps** template. Client pacing does not make the default aggregate limit sufficient for this workload.

This addresses the follow-up review of [PR 25](https://github.com/gavmac11/mumble/pull/25). The earlier [October 3 result](pr25-review-followup-2026-10-03.md) was a per-sender-only comparison at 2.5/200 Mbps, not evidence that both server defaults worked. The runbook now uses exactly two named templates unchanged; the historical hybrid remains available only as an explicitly labeled archived comparison.

## Observed results

Both runs used ten authenticated clients, two synchronized 720p30 H.264 webcam senders and ten 20 ms Opus voice senders over encrypted UDP. Each offered 30 seconds of media with two seconds to settle. Each webcam used the client-equivalent 1,972,668 bps wire allowance. Server and generator ran in separate containers limited to one CPU and 1 GiB on the same supplied VM; no test build ran concurrently.

| Video limits, sender / aggregate | Video deliveries | Voice deliveries | Complete frames captured per webcam | Overall gate |
| --- | ---: | ---: | --- | --- |
| Default, 2.5/20 Mbps | 85,680 / 130,896 | 135,000 / 135,000 | 292 and 303 of 900 each | Fail |
| Raised, 3.5/200 Mbps | 130,896 / 130,896 | 135,000 / 135,000 | 900 and 900 | Pass |

Raw records: [default result](results/2026-10-04/pr25-review2-fully-default-two-webcams.json), [default effective configuration](results/2026-10-04/pr25-review2-fully-default-two-webcams-server.ini), [raised result](results/2026-10-04/pr25-review2-raised-two-webcams.json), [raised effective configuration](results/2026-10-04/pr25-review2-raised-two-webcams-server.ini). Each effective configuration matches its repository template byte for byte after newline normalization.

Neither run had client pacing rejections, accepted-but-unsubmitted fragments, payload integrity errors, UDP receive/send-buffer error increases or CPU quota throttling. The raised run's two capture hashes match the previously strictly decoded 900-frame webcam stream. The default run's incomplete streams are a failed media result; they were not treated as valid decoded playback. [Counter summary](results/2026-10-04/comparison-summary.json).

The fixtures averaged 3.2787 Mbps of combined encoded payload, approximately 29.51 Mbps after nine-way replication, before protocol overhead. This exceeds the default 20 Mbps aggregate allowance. `Server::processVideoMsg` charges replicated traffic to the server-wide aggregate guard and silently returns when it refuses a packet. The result is consistent with that bottleneck, but no per-guard counters were added, and the two named templates change both limits. These runs do not isolate every downstream loss cause or measure GUI freezes and playout latency.

## Reproduction

Follow the [pilot runbook](host-density-pilot.md) to build the crypto and pacing bridges, create the private test endpoint, and generate the existing webcam and voice fixtures. Copy `pilot-server-defaults.ini` unchanged into the disposable runtime directory as server.ini and restart the server. Save the effective configuration and before/after UDP and cgroup counters. Use:

```sh
python3 /source/scripts/hosting/relay_probe.py \
  --host "$PILOT_PRIVATE_ADDRESS" --port 64759 --server-name localhost --ca-file /pilot.crt \
  --transport udp --crypto-library /build/libmumble_pilot_ocb.so \
  --native-pacer-library /pacer/libmumble_pilot_pacer.so \
  --clients 10 --senders 2 --fps 30 --width 1280 --height 720 \
  --h264-fixture /fixtures/webcam.h264 --video-send-cap-mbps 1.972668 \
  --voice-senders 10 --opus-fixture /fixtures/voice-noise.opus \
  --seconds 30 --settle-seconds 2 --capture-directory /results/new-default-capture
```

Repeat with `pilot-server.ini` unchanged and a fresh capture/output location for the raised-limit comparison. Preserve the failing default result. These tests restored the previous server configuration afterward; they changed no server binary or host network settings. [Source/binary hashes and restoration check](results/2026-10-04/pr25-review2-manifest.json).

## Other review resolutions

The native pacing bridge now derives its frame/packet bounds from `PacketQueue`, allowing 4,096 packets of up to 1,024 bytes rather than truncating coverage at 2,048 packets or 2 MiB. Native tests enqueue and completely drain 2,049-, 3,000- and 4,096-packet frames, and verify rejection beyond the bounds. The probe now rounds Mbps to bps; a connection-level test verifies the exact 1,972,668 bps webcam allowance. All [25 hosting/native checks](results/2026-10-04/pr25-review2-python-20261004.log) passed without skips.

Invalid-frame recovery remains conservative. An encoded frame rejected before transmission can still be an encoder reference for later P-frames. The queue has no non-reference/disposable metadata; keyframe status alone cannot establish that skipping a frame is safe. FFmpeg distinguishes [disposable, non-reference packets](https://www.ffmpeg.org/doxygen/7.1/group__lavc__packet.html), but the current capture/packetizer interface does not pass that information. The code now explains why recovery is retained, and the [pacer regression suite](results/2026-10-04/pr25-review2-ctest-final-20261004.log) verifies preserving accepted work, withholding subsequent P-frames and resuming after an IDR for invalid input. Runtime client behavior is unchanged in this follow-up.

The requested [budget-negotiation follow-up](host-density-pilot.md#budget-negotiation-follow-up) records handshake advertisement, runtime changes, aggregate allocation, legacy fallback and visible refusal/drop feedback, including a 1.5 Mbps custom-cap test. This is tracked work, not implemented negotiation. Full GUI/platform quality, external-path endurance and physical-host density remain unvalidated.
