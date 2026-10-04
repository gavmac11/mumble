// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// RAII wrapper over liboqs' ML-DSA-65 (FIPS 204) implementation, using the
// context-string API (pure mode) so signature domains stay separated
// (PQShield v2 §14).

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_SIGMLDSA65_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_SIGMLDSA65_H_

#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <oqs/oqs.h>

namespace PQFT {

class SigMLDSA65 {
public:
	/// ML-DSA-65 sizes (FIPS 204 final encodings).
	static constexpr qsizetype PublicKeySize  = 1952;
	static constexpr qsizetype SecretKeySize  = 4032;
	static constexpr qsizetype MaxSignatureSize = 3309;
	/// FIPS 204 context-string limit.
	static constexpr qsizetype MaxContextSize = 255;

	SigMLDSA65();
	~SigMLDSA65();
	SigMLDSA65(const SigMLDSA65 &)            = delete;
	SigMLDSA65 &operator=(const SigMLDSA65 &) = delete;

	bool isValid() const { return m_sig != nullptr; }

	/// Generate a long-term identity keypair.
	bool keypair(QByteArray &publicKey, SecureBytes &secretKey) const;

	/// Sign `message` with a domain-separation context (e.g. ft/handshake-v1).
	bool sign(QByteArray &signature, const SecureBytes &secretKey, const QByteArray &message,
			  const QByteArray &context) const;

	/// Verify a signature; false on any mismatch or malformed input.
	bool verify(const QByteArray &publicKey, const QByteArray &message, const QByteArray &signature,
				const QByteArray &context) const;

private:
	OQS_SIG *m_sig = nullptr;
};

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_SIGMLDSA65_H_
