# File Sharing Protocol — PQ-Shielded Transfers over Mumble (v1)

This document specifies the file-sharing feature added by this fork: arbitrary files sent
from the chat UI to every other client in the sender's channel, optionally protected by a
post-quantum password layer. It adapts the **PQShield v2 specification** ("Post-Quantum
E2E File Transfer", SUITE-PQ-HM-V1) to Mumble's client–server architecture. Where this
document copies PQShield (cryptographic suite, handshake, key schedule, manifest, password
layer, identity model), PQShield is **normative**; where the two differ, the difference is
one of the numbered adaptations below and is deliberate.

Throughout: MUST/SHOULD/MAY per RFC 2119. "Relay" means the murmur server, which is
**untrusted**: it must never learn plaintext, keys, the password, or password-derived
material, and it must never persist file data.

## 1. Adaptation summary

| # | PQShield v2 | This protocol | Why |
|---|---|---|---|
| A1 | Standalone relay protocol with framed records over TLS 1.3 to a dedicated relay | Two new Mumble TCP protobuf messages (`FileTransferControl` id 27, `FileData` id 28) relayed by murmur | Reuses Mumble's connection, framing (6-byte header, ≤ 8 MiB/message), TLS transport and client infrastructure |
| A2 | Routing tokens (random 128-bit per session) | Murmur routes by authenticated session ID (`actor` is server-set) and channel | Murmur already authenticates sessions and rewrites identity fields; tokens add nothing here |
| A3 | NACK / retransmit / 1024-entry sliding window | Omitted; strict `seq == expected` per direction on the AEAD control channel; chunks keyed by `chunk_index` | TCP is reliable and ordered; PQShield v2 §8 review explicitly endorses `seq == expected` on reliable transports |
| A4 | 1 sender → 1 recipient per session/transfer | 1 sender → N channel recipients: per-recipient pairwise handshakes + per-recipient manifests, **one** shared chunk ciphertext stream | Broadcast is the product requirement. Chunk AAD binds `transfer_digest` (§5) instead of the per-recipient `manifest_hash` |
| A5 | ML-DSA context strings via native API | Same, on liboqs ≥ 0.15 (context-parameterized signature API) | FIPS 204 final; no workaround needed at this pin. If a future pin lacks the API, the domain-prefix fallback must bump `version` |
| A6 | Relay config: framing/size/rate limits | murmur ini keys: `filebandwidth`, `filebandwidthaggregate`, `maxfilesize`, `filecontrollimit`, `filecontrolburst` (§7) | PQShield §12 semantics, murmur mechanics |

Everything else — suite, identity model and safety numbers, handshake messages M1–M4 and
transcript stages, two-stage key schedule, record framing inside control payloads, manifest
structure and Merkle rules, password double-wrap — is implemented **exactly as PQShield v2
specifies**, with PQShield's own labels (`ft/...` domain strings).

## 2. Cryptographic suite (unchanged, `SUITE-PQ-HM-V1`)

| Purpose | Primitive |
|---|---|
| Hybrid KEM | X25519 (RFC 7748) + ML-KEM-768 (FIPS 203), both ephemeral per session |
| Signatures | ML-DSA-65 (FIPS 204), long-term identity keys; contexts `ft/handshake-v1`, `ft/manifest-v1` |
| KDF | HKDF-SHA-384, two-stage extract (PQShield §7) |
| AEAD | AES-256-GCM, 96-bit nonces, full 128-bit tags |
| Password KDF | Argon2id (m=256 MiB, t=3, p=1 defaults; MIN m≥64 MiB/t≥3/p≥1, MAX m≤1 GiB/t≤10/p≤4) |
| Hash | SHA-384 |
| Encoding | Deterministic CBOR (canonical: sorted map keys, shortest-form ints, definite lengths) |

Libraries: system OpenSSL 3.x (X25519, AES-GCM, HKDF, SHA-384, HMAC), liboqs ≥ 0.15
(ML-KEM-768, ML-DSA-65), libargon2. No custom cryptographic code.

## 3. Transport messages

### `FileTransferControl` (TCP type 27) — pairwise, opaque
```
message FileTransferControl {
	optional uint32 actor = 1;            // sender's session; SET BY SERVER (anti-spoof)
	repeated uint32 target_session = 2;   // routing targets
	required bytes payload = 3;           // opaque ≤ 64 KiB (§4 framing inside)
}
```
Murmur handling: authenticated senders only; per-user leaky bucket (`filecontrollimit` /
`filecontrolburst`, defaults 20/s burst 100); payload cap 64 KiB; routes each
`target_session` that is authenticated **and in the sender's channel**; sets `actor` from
the authenticated session. No persistence. Client MUST ignore any client-supplied `actor`
value on receipt.

### `FileData` (TCP type 28) — channel broadcast
```
message FileData {
	optional uint32 actor = 1;            // sender's session; SET BY SERVER
	required bytes transfer_id = 2;       // 16 random bytes
	required uint64 chunk_index = 3;
	optional uint64 chunk_count = 4;      // hint only; the signed manifest is authoritative
	required bytes data = 5;              // AEAD ciphertext ‖ 16-byte tag, ≤ 1 MiB
}
```
Murmur handling: authenticated senders only; `transfer_id` MUST be 16 bytes and `data`
≤ 1 MiB or the message is dropped; per-user byte meter `filebandwidth` (default
16 Mbit/s) charging `20 + 8 + 6 + len(data)`; per-(session, transfer_id) in-memory
accumulator against `maxfilesize` (default 10 GiB) — exceeded ⇒ drop + log, accumulator
**never persisted**; aggregate meter `filebandwidthaggregate` (default 64 Mbit/s) charging
`packetsize × receiver_count`; broadcast to every authenticated user in the sender's
channel except the sender, **skipping users without `UserState.file_transfer_capable`**.
Chunk size: 16 KiB–1 MiB, default 256 KiB.

### Capability and limits
- `UserState.file_transfer_capable` (field 25, next free after `screen_sharing`): set by
  clients that implement this protocol; murmur echoes it in user-state broadcasts so peers
  and late joiners learn it, and uses it to gate `FileData` fan-out.
- `ServerConfig.max_file_transfer_size`: server's `maxfilesize` announced to clients.
- Stock clients never receive either message type; a stock murmur silently drops types
  27/28 (unknown switch case), so transfers against unmodified servers fail cleanly via
  handshake timeout.

## 4. Control-channel framing (inside `FileTransferControl.payload`)

2-byte header `{version:4 = 1, type:4}` followed by the body:

- Types 1–4: handshake frames **M1–M4**, canonical CBOR bodies, exactly PQShield §6
  (fields, order, transcript stages TH1/TH_hs/TH_fin/TH_full, signatures, Finished MACs).
  Cleartext at this layer (they are, however, inside Mumble's TLS + OCB2 channel crypto —
  the relay still sees them, exactly as PQShield assumes).
- Types 5–7: `CTL_MANIFEST`, `CTL_COMPLETE`, `CTL_ABORT` — post-handshake control
  records, framed `{seq(u64), type(u8), ciphertext, tag}` per PQShield §8: AES-256-GCM
  under the direction's control key, nonce = 4-byte direction prefix ‖ uint64be(seq),
  header as AAD, **strict `seq == expected`** (any gap/duplicate/reorder is fatal).
  Handshake records are only sent after both Finished MACs verified (no 0-RTT).

## 5. Transfer and broadcast key handling

Sender, per transfer (PQShield §9 with adaptation A4):

1. `transfer_id` = 16 random bytes; `file_key` = 32 random bytes (fresh, never reused);
   `chunk_nonce_prefix = HKDF-Expand(file_key, "ft/chunk-nonce/" ‖ transfer_id, 4)`.
2. Chunk the file (16 KiB–1 MiB, default 256 KiB); SHA-384 per chunk; Merkle root per
   PQShield §9.3 (`leaf = H(0x00‖chunk_hash)`, `node = H(0x01‖l‖r)`, duplicate-last-when-odd).
3. **Common manifest fields** (identical for every recipient): version, suite,
   transfer_id, fp_A, fp_B… — see §6. Define
   `transfer_digest = SHA-384(canonical CBOR of the manifest fields excluding the
   recipient-specific ones (fp_B, wrapped_file_key, signature), the informational
   created_at, and the Argon2 material (params/salt are signed manifest fields and are
   bound into the password-wrap AAD2; the password_mode flag itself IS digested)). This
   digest replaces PQShield's `manifest_hash` in the **chunk AAD** (A4); it is identical
   for all recipients so one ciphertext serves everyone.
4. Chunk records: `ct_i = AES-256-GCM(file_key, nonce = chunk_nonce_prefix ‖ uint64be(i),
   plaintext = chunk_i, AAD = canonical(transfer_id, transfer_digest, i, total_chunks,
   len_i, suite))`. Duplicate delivery of an index is fatal at the receiver (A3 removes
   any legitimate resend).
5. Per-recipient `CTL_MANIFEST` (control-encrypted): the common fields **plus** this
   recipient's `wrapped_file_key` (§6) and `sig_A` — PQShield's signature rule:
   `ML-DSA-Sign(sk_A, ctx="ft/manifest-v1", H(manifest \ {sig_A}))`.

Receiver: verify handshake → verify manifest signature and all fields (size limits,
Argon2 bounds, name sanitization) **before** Argon2 or chunk processing; recover
`file_key` (§6); accept chunks in any order, verify each GCM tag + AAD, duplicate index
⇒ fatal; completion = received set {0..n−1} + total size match + recomputed Merkle root;
write via temp file, fsync, atomic rename (PQShield §9.4); delete temp on any failure;
generic error on every failure path.

## 6. Password layer (double wrap, PQShield §10 verbatim)

Without a password the manifest carries `layer1_ct`; with a password it carries
`layer2_ct`:

```
file_key ──AES-GCM(session_kek, wrap_nonce_1, AAD1)──► layer1_ct
layer1_ct ──AES-GCM(pw_wrap_key, wrap_nonce_2, AAD2)──► layer2_ct   (only if password_mode)
```

- `session_kek = HKDF-Expand(PRK, "ft/filekey-session-wrap/" ‖ transfer_id ‖ fp_A ‖ fp_B, 32)`
  — per-recipient (derived from that pairwise session), per-transfer.
- `pw_key = Argon2id(password, salt32, m, t, p, 32)`; `pw_wrap_key = HKDF-Expand-SHA384(
  pw_key, "ft/filekey-pw-wrap/" ‖ transfer_id ‖ fp_A ‖ fp_B, 32)`.
- `wrap_nonce_1 = HKDF-Expand(session_kek, "ft/wrap-nonce-1", 12)`;
  `wrap_nonce_2 = HKDF-Expand(pw_wrap_key, "ft/wrap-nonce-2", 12)`.
- `AAD1 = canonical(transfer_id, password_mode, suite, fp_A, fp_B)`;
  `AAD2 = canonical(transfer_id, password_mode, suite, fp_A, fp_B, argon2_params, salt)`.
  AADs contain **no manifest-derived values** (no circularity; note `transfer_digest`
  deliberately does NOT appear here).
- Salt and wrapped keys travel **only** inside the encrypted `CTL_MANIFEST`.
- Argon2 params are signed manifest members; receivers enforce MIN ≤ params ≤ MAX,
  fail-closed both directions. Wrong password ⇒ GCM failure ⇒ error message identical to
  the corruption error.
- Invariant (PQShield §1.7): with a password, `file_key` requires BOTH a valid session
  AND the password — neither alone suffices.

## 7. Server (murmur) limits

| ini key | default | Meaning |
|---|---|---|
| `filebandwidth` | 16000000 | per-sender bit/s for `FileData` bytes |
| `filebandwidthaggregate` | 64000000 | server-wide relay egress bit/s |
| `maxfilesize` | 10737418240 | per-transfer byte ceiling (also announced via `ServerConfig`) |
| `filecontrollimit` | 20 | `FileTransferControl` messages/s (sustained) |
| `filecontrolburst` | 100 | `FileTransferControl` burst bucket |

The relay can drop, delay, and reorder (availability only). Reordering cannot cause an
integrity violation: every chunk is bound to its index, transfer_id, transfer_digest,
total count and length (PQShield §12 wording). The relay can never decrypt, obtain keys,
learn the password or password-derived material, remove the password requirement, forge
or replay control messages, or truncate undetected.

## 8. Identity, safety numbers, TOFU (PQShield §5 verbatim)

- Long-term ML-DSA-65 identity keypair per client; secret key encrypted at rest with an
  Argon2id-derived AES-256-GCM key; `id_pk` is the fixed 1952-byte FIPS 204 encoding.
- `fp = SHA-384("PQShield-fingerprint-v1" ‖ suite_id ‖ id_pk)`.
- Safety number `H = SHA-384("PQShield-safety-number-v1" ‖ suite_id ‖ u16len(min_fp) ‖
  min_fp ‖ u16len(max_fp) ‖ max_fp)`, lexicographic order on fingerprint bytes. QR code
  carrying full H is primary; fallback text = first 24 bytes of H as a zero-padded
  big-endian integer in 60 decimal digits, 12 groups of 5.
- First contact MUST be verified out-of-band (QR/in-person/voice) before accepting a
  transfer; TOFU pin after verification; any fingerprint change ⇒ hard block with a loud
  warning until explicitly re-verified. Pins are keyed by (server certificate digest,
  username). During the handshake, received identity keys MUST match the pin.

## 9. Client responsibilities (implementation requirements)

Zeroize secrets after use; CSPRNG only; canonical parsing everywhere (reject trailing
bytes and non-canonical CBOR); never log keys, passwords, plaintext, or full fingerprints
(log transfer IDs and coarse events); fail closed on every anomaly; no partial output
files ever.
