// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CryptoUtils.h"

#include "crypto/CryptographicRandom.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <QByteArray>

#include <cstring>

namespace PQFT {

// --- SecureBytes ---------------------------------------------------------------

SecureBytes::SecureBytes(qsizetype size) : m_data(size, Qt::Uninitialized) { }

SecureBytes SecureBytes::fromByteArray(const QByteArray &source) {
	SecureBytes result(source.size());
	std::memcpy(result.data(), source.constData(), static_cast< size_t >(source.size()));
	zeroize(const_cast< QByteArray & >(source));
	return result;
}

SecureBytes::SecureBytes(SecureBytes &&other) noexcept : m_data(std::move(other.m_data)) { }

SecureBytes &SecureBytes::operator=(SecureBytes &&other) noexcept {
	if (this != &other) {
		clear();
		m_data = std::move(other.m_data);
	}
	return *this;
}

SecureBytes::~SecureBytes() { clear(); }

unsigned char *SecureBytes::data() {
	return reinterpret_cast< unsigned char * >(m_data.data());
}

const unsigned char *SecureBytes::constData() const {
	return reinterpret_cast< const unsigned char * >(m_data.constData());
}

QByteArray SecureBytes::toByteArray() const {
	return QByteArray(m_data.constData(), static_cast< qsizetype >(m_data.size()));
}

qsizetype SecureBytes::size() const {
	return m_data.size();
}

bool SecureBytes::isEmpty() const {
	return m_data.isEmpty();
}

bool SecureBytes::operator==(const SecureBytes &other) const {
	return constantTimeEquals(m_data, other.m_data);
}

bool SecureBytes::operator!=(const SecureBytes &other) const {
	return !(*this == other);
}

void SecureBytes::clear() {
	if (!m_data.isEmpty()) {
		OPENSSL_cleanse(m_data.data(), static_cast< size_t >(m_data.size()));
		m_data.clear();
	}
}

SecureBytes SecureBytes::random(qsizetype size) {
	SecureBytes result(size);
	CryptographicRandom::fillBuffer(result.data(), static_cast< int >(size));
	return result;
}

// --- Random / zeroization --------------------------------------------------------

QByteArray randomBytes(qsizetype size) {
	QByteArray buffer(size, Qt::Uninitialized);
	CryptographicRandom::fillBuffer(buffer.data(), static_cast< int >(size));
	return buffer;
}

void zeroize(QByteArray &buffer) {
	if (!buffer.isEmpty()) {
		OPENSSL_cleanse(buffer.data(), static_cast< size_t >(buffer.size()));
	}
}

void zeroize(char *data, qsizetype size) {
	if (data && size > 0) {
		OPENSSL_cleanse(data, static_cast< size_t >(size));
	}
}

// --- Hashing ----------------------------------------------------------------------

namespace {
bool digestUpdate(EVP_MD_CTX *ctx, std::initializer_list< QByteArray > parts) {
	for (const QByteArray &part : parts) {
		if (!part.isEmpty()
			&& EVP_DigestUpdate(ctx, part.constData(), static_cast< size_t >(part.size())) != 1) {
			return false;
		}
	}
	return true;
}
} // namespace

QByteArray sha384(std::initializer_list< QByteArray > parts) {
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx)
		return QByteArray();

	QByteArray out;
	if (EVP_DigestInit_ex(ctx, EVP_sha384(), nullptr) == 1) {
		out.resize(EVP_MD_size(EVP_sha384()));
		unsigned int len = 0;
		if (digestUpdate(ctx, parts)
			&& EVP_DigestFinal_ex(ctx, reinterpret_cast< unsigned char * >(out.data()), &len) == 1) {
			out.resize(static_cast< qsizetype >(len));
		} else {
			out.clear();
		}
	}
	EVP_MD_CTX_free(ctx);
	return out;
}

QByteArray hmacSha384(const QByteArray &key, std::initializer_list< QByteArray > parts) {
	QByteArray out;

	EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
	if (!mac)
		return out;

	EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
	if (!ctx) {
		EVP_MAC_free(mac);
		return out;
	}

	const char *digestName = "SHA384";
	OSSL_PARAM params[]    = { OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
															   const_cast< char * >(digestName), 0),
						      OSSL_PARAM_construct_end() };

	bool ok = EVP_MAC_init(ctx, reinterpret_cast< const unsigned char * >(key.constData()),
						   static_cast< size_t >(key.size()), params)
			  == 1;
	for (const QByteArray &part : parts) {
		if (!ok)
			break;
		if (!part.isEmpty()
			&& EVP_MAC_update(ctx, reinterpret_cast< const unsigned char * >(part.constData()),
							  static_cast< size_t >(part.size()))
				   != 1) {
			ok = false;
		}
	}

	unsigned char result[EVP_MAX_MD_SIZE];
	size_t resultLen = 0;
	if (ok && EVP_MAC_final(ctx, result, &resultLen, sizeof(result)) == 1) {
		out = QByteArray(reinterpret_cast< const char * >(result), static_cast< qsizetype >(resultLen));
	}

	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
	return out;
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
	if (a.size() != b.size())
		return false;
	return CRYPTO_memcmp(a.constData(), b.constData(), static_cast< size_t >(a.size())) == 0;
}

// --- HKDF ------------------------------------------------------------------------

QByteArray hkdfExtract(const QByteArray &salt, const QByteArray &ikm) {
	QByteArray out(EVP_MD_get_size(EVP_sha384()), Qt::Uninitialized);

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
	if (!ctx)
		return QByteArray();

	size_t outLen = static_cast< size_t >(out.size());
	bool ok = EVP_PKEY_derive_init(ctx) == 1
			  && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha384()) == 1
			  && EVP_PKEY_CTX_set1_hkdf_salt(
					 ctx, reinterpret_cast< const unsigned char * >(salt.constData()),
					 static_cast< int >(salt.size()))
					 == 1
			  && EVP_PKEY_CTX_set1_hkdf_key(ctx,
											reinterpret_cast< const unsigned char * >(ikm.constData()),
											static_cast< int >(ikm.size()))
					 == 1
			  && EVP_PKEY_CTX_hkdf_mode(ctx, EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) == 1
			  && EVP_PKEY_derive(ctx, reinterpret_cast< unsigned char * >(out.data()), &outLen) == 1;

	EVP_PKEY_CTX_free(ctx);
	if (!ok)
		return QByteArray();
	out.resize(static_cast< qsizetype >(outLen));
	return out;
}

QByteArray hkdfExpand(const QByteArray &prk, const QByteArray &info, qsizetype length) {
	if (length <= 0)
		return QByteArray();

	QByteArray out(length, Qt::Uninitialized);

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
	if (!ctx)
		return QByteArray();

	size_t outLen = static_cast< size_t >(out.size());
	bool ok = EVP_PKEY_derive_init(ctx) == 1
			  && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha384()) == 1
			  && EVP_PKEY_CTX_set1_hkdf_key(ctx,
											reinterpret_cast< const unsigned char * >(prk.constData()),
											static_cast< int >(prk.size()))
					 == 1
			  && EVP_PKEY_CTX_add1_hkdf_info(
					 ctx, reinterpret_cast< const unsigned char * >(info.constData()),
					 static_cast< int >(info.size()))
					 == 1
			  && EVP_PKEY_CTX_hkdf_mode(ctx, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) == 1
			  && EVP_PKEY_derive(ctx, reinterpret_cast< unsigned char * >(out.data()), &outLen) == 1;

	EVP_PKEY_CTX_free(ctx);
	if (!ok)
		return QByteArray();
	out.resize(static_cast< qsizetype >(outLen));
	return out;
}

QByteArray hkdfSha384(const QByteArray &salt, const QByteArray &ikm, const QByteArray &info,
					  qsizetype length) {
	const QByteArray prk = hkdfExtract(salt, ikm);
	if (prk.isEmpty())
		return QByteArray();
	return hkdfExpand(prk, info, length);
}

// --- X25519 -----------------------------------------------------------------------

bool x25519GenerateKeyPair(QByteArray &publicKey, SecureBytes &secretKey) {
	publicKey.clear();
	secretKey.clear();

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
	if (!ctx)
		return false;

	EVP_PKEY *key = nullptr;
	bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!ok || !key)
		return false;

	size_t pubLen = 32, secLen = 32;
	publicKey.resize(32);
	secretKey  = SecureBytes(32);
	ok = EVP_PKEY_get_raw_public_key(key, reinterpret_cast< unsigned char * >(publicKey.data()), &pubLen)
			   == 1
		 && EVP_PKEY_get_raw_private_key(key, secretKey.data(), &secLen) == 1;
	EVP_PKEY_free(key);
	if (!ok || pubLen != 32 || secLen != 32) {
		zeroize(publicKey);
		publicKey.clear();
		secretKey.clear();
		return false;
	}
	return true;
}

QByteArray x25519SharedSecret(const QByteArray &ourSecretKey, const QByteArray &peerPublicKey) {
	if (ourSecretKey.size() != 32 || peerPublicKey.size() != 32)
		return QByteArray();

	QByteArray out;

	EVP_PKEY *ourKey = EVP_PKEY_new_raw_private_key(
		EVP_PKEY_X25519, nullptr,
		reinterpret_cast< const unsigned char * >(ourSecretKey.constData()), 32);
	if (!ourKey)
		return out;

	EVP_PKEY *peerKey = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_X25519, nullptr,
		reinterpret_cast< const unsigned char * >(peerPublicKey.constData()), 32);
	if (peerKey) {
		EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(ourKey, nullptr);
		if (ctx) {
			if (EVP_PKEY_derive_init(ctx) == 1
				&& EVP_PKEY_derive_set_peer(ctx, peerKey) == 1) {
				size_t secretLen = 32;
				out.resize(32);
				if (EVP_PKEY_derive(ctx, reinterpret_cast< unsigned char * >(out.data()), &secretLen)
					!= 1) {
					out.clear();
				} else {
					out.resize(static_cast< qsizetype >(secretLen));
				}
			}
			EVP_PKEY_CTX_free(ctx);
		}
		EVP_PKEY_free(peerKey);
	}
	EVP_PKEY_free(ourKey);
	return out;
}

// --- AES-256-GCM -------------------------------------------------------------------

bool isAllZero(const QByteArray &buffer) {
	char accumulator = 0;
	for (qsizetype i = 0; i < buffer.size(); ++i) {
		accumulator |= buffer.at(i);
	}
	return accumulator == 0;
}


bool aesGcmEncrypt(QByteArray &out, const QByteArray &key, const QByteArray &nonce,
				   const QByteArray &plaintext, const QByteArray &aad) {
	out.clear();
	if (key.size() != 32 || nonce.size() != 12)
		return false;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
			  && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
			  && EVP_EncryptInit_ex(
					 ctx, nullptr, nullptr, reinterpret_cast< const unsigned char * >(key.constData()),
					 reinterpret_cast< const unsigned char * >(nonce.constData()))
					 == 1;

	if (ok && !aad.isEmpty()) {
		int len = 0;
		ok = EVP_EncryptUpdate(ctx, nullptr, &len,
							   reinterpret_cast< const unsigned char * >(aad.constData()),
							   static_cast< int >(aad.size()))
			  == 1;
	}

	if (ok) {
		out.resize(plaintext.size() + 16);
		int written = 0;
		int total   = 0;
		if (!plaintext.isEmpty()) {
			ok = EVP_EncryptUpdate(ctx, reinterpret_cast< unsigned char * >(out.data()), &written,
								   reinterpret_cast< const unsigned char * >(plaintext.constData()),
								   static_cast< int >(plaintext.size()))
				 == 1;
			total = written;
		}
		if (ok) {
			ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast< unsigned char * >(out.data()) + total,
									 &written)
				 == 1;
			total += written;
		}
		if (ok) {
			ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
									 reinterpret_cast< unsigned char * >(out.data()) + total)
				 == 1;
		}
		if (!ok)
			out.clear();
		else
			out.resize(static_cast< qsizetype >(total + 16));
	}

	EVP_CIPHER_CTX_free(ctx);
	return ok;
}

bool aesGcmDecrypt(QByteArray &out, const QByteArray &key, const QByteArray &nonce,
				   const QByteArray &ciphertextAndTag, const QByteArray &aad) {
	out.clear();
	if (key.size() != 32 || nonce.size() != 12 || ciphertextAndTag.size() < 16)
		return false;

	const qsizetype ciphertextLen = ciphertextAndTag.size() - 16;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
			  && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
			  && EVP_DecryptInit_ex(
					 ctx, nullptr, nullptr, reinterpret_cast< const unsigned char * >(key.constData()),
					 reinterpret_cast< const unsigned char * >(nonce.constData()))
					 == 1;

	if (ok && !aad.isEmpty()) {
		int len = 0;
		ok = EVP_DecryptUpdate(ctx, nullptr, &len,
							   reinterpret_cast< const unsigned char * >(aad.constData()),
							   static_cast< int >(aad.size()))
			  == 1;
	}

	if (ok) {
		QByteArray tag = ciphertextAndTag.right(16);
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
								 reinterpret_cast< unsigned char * >(
									 const_cast< char * >(tag.constData())))
			 == 1;
	}

	if (ok) {
		out.resize(ciphertextLen > 0 ? ciphertextLen : 0);
		int written = 0;
		int total   = 0;
		if (ciphertextLen > 0) {
			ok = EVP_DecryptUpdate(ctx, reinterpret_cast< unsigned char * >(out.data()), &written,
								   reinterpret_cast< const unsigned char * >(
									   ciphertextAndTag.constData()),
								   static_cast< int >(ciphertextLen))
				 == 1;
			total = written;
		}
		if (ok) {
			ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast< unsigned char * >(out.data()) + total,
									 &written)
				 == 1;
		}
		if (!ok)
			out.clear();
	}

	EVP_CIPHER_CTX_free(ctx);
	return ok;
}

QByteArray uint64be(quint64 value) {
	QByteArray out(8, Qt::Uninitialized);
	for (int i = 7; i >= 0; --i) {
		out[static_cast< qsizetype >(7 - i)] = static_cast< char >((value >> (8 * i)) & 0xff);
	}
	return out;
}

QByteArray uint32be(quint32 value) {
	QByteArray out(4, Qt::Uninitialized);
	for (int i = 3; i >= 0; --i) {
		out[static_cast< qsizetype >(3 - i)] = static_cast< char >((value >> (8 * i)) & 0xff);
	}
	return out;
}

} // namespace PQFT
