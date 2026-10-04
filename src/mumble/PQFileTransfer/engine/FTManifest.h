// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Signed transfer manifest and chunk record helpers (PROTOCOL.md §5,
// PQShield v2 §9 with the broadcast adaptation): one chunk ciphertext is
// shared by all recipients, manifests are per-recipient, and the chunk AAD
// binds transfer_digest — the SHA-384 of the manifest's recipient-independent
// fields — instead of a per-recipient manifest hash.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMANIFEST_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMANIFEST_H_

#include "PQFileTransfer/crypto/Argon2Wrap.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

namespace PQFT {

struct FTManifest {
	// common fields
	quint8 version		  = 1;
	QByteArray transferId;   // 16 B
	QByteArray fpA;		  // sender fingerprint (48 B)
	QByteArray fpB;		  // recipient fingerprint (48 B)
	QString fileName;	   // basename only, ≤255 UTF-8 bytes
	QString mimeType;
	quint64 fileSize		  = 0;
	quint32 chunkSize		  = 0;
	quint64 chunkCount		  = 0;
	QByteArray merkleRoot;   // 48 B
	bool passwordMode		  = false;
	std::optional< Argon2Params > argon2;
	QByteArray argon2Salt;   // 32 B, only in password mode
	QByteArray wrappedFileKey; // layer1_ct (no password) or layer2_ct (password)
	quint64 createdAtUnix	  = 0;
	QByteArray signature;	// ML-DSA-65 over the sig-less manifest

	/// SHA-384 over the canonical CBOR of every field except the
	/// recipient-specific ones (fp_B, wrapped_file_key, signature). Identical
	/// for all recipients of a transfer; anchors the chunk AAD.
	QByteArray transferDigest() const;
};

/// Build the canonical CBOR encoding of the manifest (all fields).
QByteArray encodeManifest(const FTManifest &manifest);

/// Canonical CBOR of the manifest without the signature field — the signed
/// payload.
QByteArray manifestBody(const FTManifest &manifest);

/// Sign a manifest with the sender identity: sig = ML-DSA(sk, ctx
/// ft/manifest-v1, H(body)). Returns false on failure.
bool signManifest(FTManifest &manifest,
				  const std::function< bool(QByteArray &, const QByteArray &, const QByteArray &) > &sign);

/// Strict parse + full verification. `senderFingerprint` is the peer
/// fingerprint verified by the handshake session; `maxFileSize` the local
/// receive ceiling. Checks canonical form, sizes, Argon2 bounds, name
/// sanitization, fp_A == senderFingerprint and the signature — all BEFORE
/// any password work. Fail-closed.
bool parseAndVerifyManifest(FTManifest &out, const QByteArray &encoded,
							const QByteArray &senderFingerprint, quint64 maxFileSize,
							const std::function< bool(const QByteArray & /*message*/,
													  const QByteArray & /*signature*/,
													  const QByteArray & /*context*/) > &verify);

/// chunk_nonce_prefix = HKDF-Expand(file_key, "ft/chunk-nonce/" ‖ transfer_id, 4)
QByteArray chunkNoncePrefix(const QByteArray &fileKey, const QByteArray &transferId);

/// AAD binding one chunk: canonical(transfer_id, transfer_digest, index,
/// total, length, suite).
QByteArray chunkAAD(const QByteArray &transferId, const QByteArray &transferDigest, quint64 index,
					quint64 total, quint32 length);

/// Encrypt one chunk (ct = ciphertext ‖ tag). Nonce is structural.
bool encryptChunk(QByteArray &out, const QByteArray &fileKey, const QByteArray &transferId,
				  const QByteArray &transferDigest, quint64 index, quint64 total,
				  const QByteArray &chunk);

/// Verify + decrypt one chunk (duplicate/out-of-range handling is the
/// caller's job; any AEAD failure here is fatal).
bool decryptChunk(QByteArray &out, const QByteArray &fileKey, const QByteArray &transferId,
				  const QByteArray &transferDigest, quint64 index, quint64 total,
				  const QByteArray &ciphertextAndTag);

// --- File-key wrapping (§10) ------------------------------------------------------

/// Layer 1 (always): file_key -> AES-GCM(session_kek, wrap_nonce_1, AAD1).
bool wrapFileKeySessionLayer(QByteArray &layer1Ct, const QByteArray &fileKey,
							 const QByteArray &sessionKek, const QByteArray &wrapNonce1,
							 const QByteArray &transferId, bool passwordMode,
							 const QByteArray &fpA, const QByteArray &fpB);

/// Unwrap layer 1.
bool unwrapFileKeySessionLayer(QByteArray &fileKey, const QByteArray &layer1Ct,
							   const QByteArray &sessionKek, const QByteArray &wrapNonce1,
							   const QByteArray &transferId, bool passwordMode,
							   const QByteArray &fpA, const QByteArray &fpB);

/// Layer 2 (password mode only): layer1_ct -> AES-GCM(pw_wrap_key,
/// wrap_nonce_2, AAD2). pwWrapKey comes from Argon2id + HKDF (§10).
QByteArray passwordWrapKey(const QByteArray &pwKey, const QByteArray &transferId,
						   const QByteArray &fpA, const QByteArray &fpB);
QByteArray wrapNonce2(const QByteArray &pwWrapKey);

bool wrapLayer1WithPassword(QByteArray &layer2Ct, const QByteArray &layer1Ct,
							const QByteArray &pwWrapKey, const QByteArray &transferId,
							bool passwordMode, const QByteArray &fpA, const QByteArray &fpB,
							const Argon2Params &params, const QByteArray &salt);
bool unwrapPasswordLayer(QByteArray &layer1Ct, const QByteArray &layer2Ct,
						 const QByteArray &pwWrapKey, const QByteArray &transferId,
						 bool passwordMode, const QByteArray &fpA, const QByteArray &fpB,
						 const Argon2Params &params, const QByteArray &salt);

/// The one generic error for wrong password and corruption alike.
QString genericDecryptionError();

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMANIFEST_H_
