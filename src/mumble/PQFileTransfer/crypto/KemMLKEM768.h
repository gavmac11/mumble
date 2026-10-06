// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// RAII wrapper over liboqs' ML-KEM-768 (FIPS 203) implementation.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_KEMMLKEM768_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_KEMMLKEM768_H_

#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <oqs/oqs.h>

namespace PQFT {

/// Thin fail-closed wrapper around OQS_KEM for ML-KEM-768. A single instance
/// is not thread-safe; use one per session/job (construction is cheap).
class KemMLKEM768 {
public:
	/// Key sizes on the wire (ML-KEM-768).
	static constexpr qsizetype PublicKeySize  = 1184;
	static constexpr qsizetype SecretKeySize  = 2400;
	static constexpr qsizetype CiphertextSize = 1088;
	static constexpr qsizetype SharedSecretSize = 32;

	KemMLKEM768();
	~KemMLKEM768();
	KemMLKEM768(const KemMLKEM768 &)            = delete;
	KemMLKEM768 &operator=(const KemMLKEM768 &) = delete;

	bool isValid() const { return m_kem != nullptr; }

	/// Generate an ephemeral keypair. Secret key material is returned in a
	/// zeroizing container.
	bool keypair(QByteArray &publicKey, SecureBytes &secretKey) const;

	/// Encapsulate against `peerPublicKey`: produces the KEM ciphertext to
	/// send and the shared secret.
	bool encaps(QByteArray &ciphertext, SecureBytes &sharedSecret, const QByteArray &peerPublicKey) const;

	/// Decapsulate `ciphertext` with `secretKey`. Standard implicit rejection
	/// applies inside liboqs: a malformed ciphertext still yields a
	/// pseudorandom secret (no branching on failure) — callers simply feed
	/// the result into the KDF.
	bool decaps(SecureBytes &sharedSecret, const QByteArray &ciphertext, const SecureBytes &secretKey) const;

private:
	OQS_KEM *m_kem = nullptr;
};

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_KEMMLKEM768_H_
