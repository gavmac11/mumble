// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Thin fail-closed wrappers over the OpenSSL EVP interfaces used by the
// file-transfer crypto core (PROTOCOL.md §2). No custom cryptography.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CRYPTOUTILS_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CRYPTOUTILS_H_

#include <QByteArray>

namespace PQFT {

/// QByteArray-like container for secret material that zeroizes its buffer
/// on destruction and on clear(). Movable, not copyable (copying would
/// multiply secret buffers beyond controlled zeroization).
class SecureBytes {
public:
	SecureBytes() = default;
	/// Allocate size bytes of UNINITIALIZED storage.
	explicit SecureBytes(qsizetype size);
	static SecureBytes fromByteArray(const QByteArray &source);

	SecureBytes(const SecureBytes &)            = delete;
	SecureBytes &operator=(const SecureBytes &) = delete;
	SecureBytes(SecureBytes &&other) noexcept;
	SecureBytes &operator=(SecureBytes &&other) noexcept;
	~SecureBytes();

	unsigned char *data();
	const unsigned char *constData() const;
	QByteArray toByteArray() const;
	qsizetype size() const;
	bool isEmpty() const;
	bool operator==(const SecureBytes &other) const;
	bool operator!=(const SecureBytes &other) const;

	/// Overwrite with zeros and drop the buffer.
	void clear();

	/// SecureBytes of n cryptographically random bytes.
	static SecureBytes random(qsizetype size);

private:
	QByteArray m_data;
};

/// Cryptographically random bytes (OS entropy via CryptographicRandom).
QByteArray randomBytes(qsizetype size);

/// Zeroize a QByteArray's buffer (use on transient key copies).
void zeroize(QByteArray &buffer);
void zeroize(char *data, qsizetype size);

/// SHA-384 over the concatenation of parts.
QByteArray sha384(std::initializer_list< QByteArray > parts);

/// HMAC-SHA-384 of message under key.
QByteArray hmacSha384(const QByteArray &key, std::initializer_list< QByteArray > parts);

/// Constant-time comparison.
bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

/// HKDF-SHA-384 Extract (the salt and IKM are hashed to a fixed PRK).
QByteArray hkdfExtract(const QByteArray &salt, const QByteArray &ikm);

/// HKDF-SHA-384 Expand to `length` bytes.
QByteArray hkdfExpand(const QByteArray &prk, const QByteArray &info, qsizetype length);

/// Convenience: Extract-then-Expand in one call.
QByteArray hkdfSha384(const QByteArray &salt, const QByteArray &ikm, const QByteArray &info,
					  qsizetype length);

/// Generate an ephemeral X25519 keypair (clamped secret via OpenSSL keygen).
bool x25519GenerateKeyPair(QByteArray &publicKey, SecureBytes &secretKey);

/// X25519 scalar multiplication. The result may be all zeros when the peer
/// point has small order; callers MUST treat that as fatal (RFC 7748 §6.1),
/// e.g. via isAllZero().
QByteArray x25519SharedSecret(const QByteArray &ourSecretKey, const QByteArray &peerPublicKey);

/// True when every byte of `buffer` is zero (small-order X25519 check).
bool isAllZero(const QByteArray &buffer);

/// AES-256-GCM encrypt: output = ciphertext || 16-byte tag. Empty plaintext
/// is allowed (then only the tag is produced).
bool aesGcmEncrypt(QByteArray &out, const QByteArray &key, const QByteArray &nonce,
				   const QByteArray &plaintext, const QByteArray &aad);

/// AES-256-GCM decrypt of `ciphertextAndTag`. Returns false on ANY failure
/// (tag mismatch, wrong sizes); on failure `out` is left empty.
bool aesGcmDecrypt(QByteArray &out, const QByteArray &key, const QByteArray &nonce,
				   const QByteArray &ciphertextAndTag, const QByteArray &aad);

/// Big-endian encoding of a 64-bit counter (nonce suffixes, indices).
QByteArray uint64be(quint64 value);
/// Big-endian encoding of a 32-bit value.
QByteArray uint32be(quint32 value);

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CRYPTOUTILS_H_
