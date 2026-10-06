// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/SigMLDSA65.h"

namespace PQFT {

SigMLDSA65::SigMLDSA65() {
	m_sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
}

SigMLDSA65::~SigMLDSA65() {
	if (m_sig) {
		OQS_SIG_free(m_sig);
		m_sig = nullptr;
	}
}

bool SigMLDSA65::keypair(QByteArray &publicKey, SecureBytes &secretKey) const {
	publicKey.clear();
	secretKey.clear();
	if (!m_sig)
		return false;

	publicKey.resize(static_cast< qsizetype >(m_sig->length_public_key));
	secretKey  = SecureBytes(static_cast< qsizetype >(m_sig->length_secret_key));
	if (OQS_SIG_keypair(m_sig, reinterpret_cast< unsigned char * >(publicKey.data()),
						secretKey.data())
		!= OQS_SUCCESS) {
		publicKey.clear();
		secretKey.clear();
		return false;
	}
	return true;
}

bool SigMLDSA65::sign(QByteArray &signature, const SecureBytes &secretKey, const QByteArray &message,
					  const QByteArray &context) const {
	signature.clear();
	if (!m_sig || secretKey.size() != static_cast< qsizetype >(m_sig->length_secret_key)
		|| context.size() > MaxContextSize)
		return false;

	signature.resize(static_cast< qsizetype >(m_sig->length_signature));
	size_t signatureLen = 0;
	if (OQS_SIG_sign_with_ctx_str(
			m_sig, reinterpret_cast< unsigned char * >(signature.data()), &signatureLen,
			reinterpret_cast< const unsigned char * >(message.constData()),
			static_cast< size_t >(message.size()),
			reinterpret_cast< const unsigned char * >(context.constData()),
			static_cast< size_t >(context.size()), secretKey.constData())
		!= OQS_SUCCESS) {
		signature.clear();
		return false;
	}
	signature.resize(static_cast< qsizetype >(signatureLen));
	return true;
}

bool SigMLDSA65::verify(const QByteArray &publicKey, const QByteArray &message,
						const QByteArray &signature, const QByteArray &context) const {
	if (!m_sig || publicKey.size() != static_cast< qsizetype >(m_sig->length_public_key)
		|| context.size() > MaxContextSize)
		return false;

	return OQS_SIG_verify_with_ctx_str(
			   m_sig, reinterpret_cast< const unsigned char * >(message.constData()),
			   static_cast< size_t >(message.size()),
			   reinterpret_cast< const unsigned char * >(signature.constData()),
			   static_cast< size_t >(signature.size()),
			   reinterpret_cast< const unsigned char * >(context.constData()),
			   static_cast< size_t >(context.size()),
			   reinterpret_cast< const unsigned char * >(publicKey.constData()))
		   == OQS_SUCCESS;
}

} // namespace PQFT
