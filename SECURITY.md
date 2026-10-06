# Security Notes — Chat File Transfer

This file documents the security properties and deliberate limitations of the
post-quantum chat file-transfer feature. The wire protocol is specified in
[PROTOCOL.md](PROTOCOL.md), an adaptation of the PQShield v2 specification
(`SUITE-PQ-HM-V1`). Nothing here overrides that spec; this file states what
the implementation does and does not promise.

## What the password layer protects — and what it does not

- With a password set, recovering the file key requires **both** an
  authenticated pairwise session (the session-derived KEK, layer 1) **and**
  the password (the Argon2id-derived KEK, layer 2). Neither factor alone
  decrypts (PQShield v2 §1.7).
- The password layer is defense-in-depth on top of the session. It protects
  the file when a session transcript or exported transfer bundle is exposed
  **short of session-key compromise**.
- **Known limit:** an attacker who obtains the session keys *and* the
  manifest can mount an offline guessing attack against the password. Use
  long, high-entropy passphrases.
- **Consequence:** a completed transfer cannot be re-decrypted later from a
  saved bundle after session keys are destroyed. Persistent re-decryptable
  archives would be an explicit opt-in future feature (receiver re-wraps the
  file key under a locally stored key).

## Forward secrecy — the precise claim

The handshake uses ephemeral X25519 and ephemeral ML-KEM-768 keys with
long-term ML-DSA-65 authentication. Compromise of a client **after** the
ephemeral session secrets have been securely destroyed does not by itself
recover previously completed sessions, assuming the cryptographic primitives
remain secure. Compromise **during** a session voids that session's
confidentiality.

## Non-repudiation

Manifests are signed with the sender's long-term ML-DSA-65 key. This is a
deliberate tradeoff: a signed manifest is cryptographic evidence that its
issuer authorized that transfer. Deniability-preserving constructions were
explicitly rejected for v1 (PQShield v2 §7/§16).

## What the server (murmur) can and cannot do

The relay is untrusted by design. It **can**: drop, delay and reorder
delivery (an availability property only — clients detect all three); see
message types, sizes, timing, and who talks to whom (no traffic-analysis
resistance is provided). It **cannot**: decrypt content, obtain the file key
(double-wrapped outside its view), learn the password or password-derived
material, remove the password requirement (signed manifest + AAD binding),
modify data undetected (AEAD + AAD), substitute identities (signatures +
TOFU pins), forge or replay control messages (strict sequencing + AEAD), or
truncate undetected (signed manifest + Merkle root). The server never
persists file data; per-transfer byte accounting is in-memory only.

## Identity changes are hard blocks

Fingerprints are pinned per (server certificate digest, username) on first
verified contact. If a peer later presents a different identity key, the
transfer is blocked and a loud warning is shown; there are no silent key
updates. First-use pinning on the sending side pins the peer's
signature-verified key when no pin exists yet and immediately offers the
safety-number check — a documented TOFU softening of the strict
verify-before-first-transfer rule on the initiator side only. The receiving
side always requires explicit verification before a transfer proceeds: an
incoming M1 presents an unsigned key claim, so nothing is written to the
trust store until the safety-number dialog is accepted; declining leaves no
pin behind and the handshake is never answered.

## Operational notes

- Wrong passwords and corrupted data produce one identical generic error —
  there is no oracle.
- Secrets (session keys, file keys, password-derived keys, identity keys)
  are zeroized after use; the identity secret key is stored encrypted at
  rest under an Argon2id-derived AES-256-GCM key.
- Server limits (`filebandwidth`, `filebandwidthaggregate`, `maxfilesize`,
  `filecontrollimit`, `filecontrolburst`) bound abuse but are not a
  substitute for transport security expectations.
