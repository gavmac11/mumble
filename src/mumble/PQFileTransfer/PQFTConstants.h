// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// This file is part of the file-sharing feature added by this fork.
// See PROTOCOL.md at the repository root for the wire protocol this
// implements (an adaptation of the PQShield v2 specification).

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_PQFTCONSTANTS_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_PQFTCONSTANTS_H_

#include <QtGlobal>

/// Constants of the SUITE-PQ-HM-V1 file-transfer suite (PROTOCOL.md §2).
/// All values are normative; changing any of them is a protocol break.
namespace PQFT {

/// Protocol version (frame header carries this in the upper nibble).
constexpr quint8 Version = 1;

/// The one and only suite identifier (downgrade protection by construction).
constexpr char SuiteId[] = "SUITE-PQ-HM-V1";

// --- Domain-separation labels (PQShield v2 §7/§9/§10 — exact) ---------------

/// Handshake transcript-signature context.
constexpr char LabelHandshakeCtx[] = "ft/handshake-v1";
/// Manifest signature context.
constexpr char LabelManifestCtx[] = "ft/manifest-v1";

/// Direction-separated finished-MAC keys (a2b = sender A to recipient B).
constexpr char LabelFinishedA2B[] = "ft/finished/a2b";
constexpr char LabelFinishedB2A[] = "ft/finished/b2a";
/// Direction-separated traffic secrets.
constexpr char LabelTrafficA2B[]   = "ft/traffic/a2b";
constexpr char LabelTrafficB2A[]   = "ft/traffic/b2a";
/// Per-traffic-secret purposes.
constexpr char LabelControlKey[]   = "ft/control-key";
constexpr char LabelControlNonce[] = "ft/control-nonce";
/// Session KEK that wraps the file key (layer 1).
constexpr char LabelSessionWrap[]  = "ft/filekey-session-wrap/";
/// Nonce for the session wrap (derived from the session KEK).
constexpr char LabelWrapNonce1[]   = "ft/wrap-nonce-1";
/// Password KEK that wraps the session-wrapped file key (layer 2).
constexpr char LabelPwWrap[]       = "ft/filekey-pw-wrap/";
/// Nonce for the password wrap (derived from the password KEK).
constexpr char LabelWrapNonce2[]   = "ft/wrap-nonce-2";
/// Per-transfer chunk-nonce prefix (derived from the file key).
constexpr char LabelChunkNonce[]   = "ft/chunk-nonce/";

/// Identity fingerprint domain string (PQShield v2 §5).
constexpr char LabelFingerprint[]  = "PQShield-fingerprint-v1";
/// Safety-number domain string (PQShield v2 §5).
constexpr char LabelSafetyNumber[] = "PQShield-safety-number-v1";

// --- Sizes --------------------------------------------------------------------

/// SHA-384 digest length in bytes.
constexpr qsizetype HashSize = 48;
/// Symmetric key length in bytes (AES-256 / HKDF output).
constexpr qsizetype KeySize = 32;
/// AES-GCM nonce length in bytes.
constexpr qsizetype NonceSize = 12;
/// AES-GCM authentication tag length in bytes (truncation forbidden).
constexpr qsizetype TagSize = 16;
/// Finished-MAC key length in bytes (HMAC-SHA-384 key material).
constexpr qsizetype FinishedKeySize = 48;
/// Traffic secret length in bytes.
constexpr qsizetype TrafficSecretSize = 48;
/// Random transfer identifier length in bytes.
constexpr qsizetype TransferIdSize = 16;
/// Argon2id salt length in bytes.
constexpr qsizetype Argon2SaltSize = 32;
/// Argon2id output length in bytes.
constexpr qsizetype Argon2OutputSize = 32;
/// Random handshake nonce length in bytes.
constexpr qsizetype HandshakeNonceSize = 32;

// X25519
constexpr qsizetype X25519KeySize = 32;

// --- Limits --------------------------------------------------------------------

/// Maximum payload of a FileTransferControl message (murmur enforces this too).
constexpr qsizetype MaxControlPayload = 64 * 1024;
/// Minimum chunk plaintext size.
constexpr qsizetype MinChunkSize = 16 * 1024;
/// Maximum chunk plaintext size (murmur enfaces the resulting frame size).
constexpr qsizetype MaxChunkSize = 1024 * 1024;
/// Default chunk plaintext size.
constexpr qsizetype DefaultChunkSize = 256 * 1024;
/// Default maximum accepted file size (10 GiB, PROTOCOL.md §7).
constexpr quint64 DefaultMaxFileSize = 10ull * 1024 * 1024 * 1024;
/// Maximum file name length in bytes (basename only, UTF-8).
constexpr qsizetype MaxFileNameBytes = 255;

// --- Argon2id parameter bounds (PQShield v2 §10; fail closed both ways) ------

/// Minimum accepted Argon2id memory cost (64 MiB, in KiB as libargon2 expects).
constexpr quint32 Argon2MinMemoryKiB = 64 * 1024;
/// Default Argon2id memory cost (256 MiB, in KiB).
constexpr quint32 Argon2DefaultMemoryKiB = 256 * 1024;
/// Maximum accepted Argon2id memory cost (1 GiB, in KiB).
constexpr quint32 Argon2MaxMemoryKiB = 1024 * 1024;
/// Minimum accepted Argon2id time cost.
constexpr quint32 Argon2MinTimeCost = 3;
/// Default Argon2id time cost.
constexpr quint32 Argon2DefaultTimeCost = 3;
/// Maximum accepted Argon2id time cost.
constexpr quint32 Argon2MaxTimeCost = 10;
/// Minimum accepted Argon2id parallelism.
constexpr quint32 Argon2MinParallelism = 1;
/// Default Argon2id parallelism.
constexpr quint32 Argon2DefaultParallelism = 1;
/// Maximum accepted Argon2id parallelism.
constexpr quint32 Argon2MaxParallelism = 4;

/// Minimum password length enforced on the sender side.
constexpr qsizetype MinPasswordLength = 8;

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_PQFTCONSTANTS_H_
