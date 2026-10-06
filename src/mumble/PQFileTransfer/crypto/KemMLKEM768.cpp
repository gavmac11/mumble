// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/KemMLKEM768.h"

namespace PQFT {

KemMLKEM768::KemMLKEM768() {
	m_kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_768);
}

KemMLKEM768::~KemMLKEM768() {
	if (m_kem) {
		OQS_KEM_free(m_kem);
		m_kem = nullptr;
	}
}

bool KemMLKEM768::keypair(QByteArray &publicKey, SecureBytes &secretKey) const {
	publicKey.clear();
	secretKey.clear();
	if (!m_kem)
		return false;

	publicKey.resize(static_cast< qsizetype >(m_kem->length_public_key));
	secretKey  = SecureBytes(static_cast< qsizetype >(m_kem->length_secret_key));
	if (OQS_KEM_keypair(m_kem, reinterpret_cast< unsigned char * >(publicKey.data()),
						secretKey.data())
		!= OQS_SUCCESS) {
		publicKey.clear();
		secretKey.clear();
		return false;
	}
	return true;
}

bool KemMLKEM768::encaps(QByteArray &ciphertext, SecureBytes &sharedSecret,
						 const QByteArray &peerPublicKey) const {
	ciphertext.clear();
	sharedSecret.clear();
	if (!m_kem || peerPublicKey.size() != static_cast< qsizetype >(m_kem->length_public_key))
		return false;

	ciphertext.resize(static_cast< qsizetype >(m_kem->length_ciphertext));
	sharedSecret = SecureBytes(static_cast< qsizetype >(m_kem->length_shared_secret));
	if (OQS_KEM_encaps(m_kem, reinterpret_cast< unsigned char * >(ciphertext.data()),
					   sharedSecret.data(),
					   reinterpret_cast< const unsigned char * >(peerPublicKey.constData()))
		!= OQS_SUCCESS) {
		ciphertext.clear();
		sharedSecret.clear();
		return false;
	}
	return true;
}

bool KemMLKEM768::decaps(SecureBytes &sharedSecret, const QByteArray &ciphertext,
						 const SecureBytes &secretKey) const {
	sharedSecret.clear();
	if (!m_kem || ciphertext.size() != static_cast< qsizetype >(m_kem->length_ciphertext)
		|| secretKey.size() != static_cast< qsizetype >(m_kem->length_secret_key))
		return false;

	sharedSecret = SecureBytes(static_cast< qsizetype >(m_kem->length_shared_secret));
	// Implicit rejection: a bad ciphertext yields a pseudorandom secret rather
	// than an error; that value flows into the KDF and the handshake's MACs
	// fail later (PROTOCOL.md §2, PQShield v2 §4).
	if (OQS_KEM_decaps(m_kem, sharedSecret.data(),
					   reinterpret_cast< const unsigned char * >(ciphertext.constData()),
					   secretKey.constData())
		!= OQS_SUCCESS) {
		sharedSecret.clear();
		return false;
	}
	return true;
}

} // namespace PQFT
