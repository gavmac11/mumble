// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Identity layer of the file-transfer feature (PROTOCOL.md §8, PQShield v2
// §5): fingerprints, safety numbers (QR primary, 60-digit fallback) and the
// encrypted-at-rest long-term ML-DSA-65 identity keypair.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_FTIDENTITY_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_FTIDENTITY_H_

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/Argon2Wrap.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <QByteArray>
#include <QDate>
#include <QSqlDatabase>
#include <QString>

namespace PQFT {

/// fp = SHA-384("PQShield-fingerprint-v1" ‖ suite_id ‖ id_pk)
QByteArray identityFingerprint(const QByteArray &mldsaPublicKey);

/// H = SHA-384("PQShield-safety-number-v1" ‖ suite_id ‖
///            u16len(min_fp) ‖ min_fp ‖ u16len(max_fp) ‖ max_fp)
/// with lexicographic byte order on the fingerprints.
QByteArray safetyNumberHash(const QByteArray &fingerprintA, const QByteArray &fingerprintB);

/// First 24 bytes of the safety-number hash as a zero-padded big-endian
/// decimal number in 12 groups of 5 digits.
QString formatSafetyNumberDigits(const QByteArray &safetyHash);

/// Convenience: formatted safety number for a fingerprint pair.
QString safetyNumber(const QByteArray &fingerprintA, const QByteArray &fingerprintB);

/// Canonical-CBOR QR payload carrying the FULL 48-byte hash (primary
/// verification channel): map { 1: version, 2: suite, 3: min_fp, 4: max_fp,
/// 5: hash }. Empty QByteArray on failure.
QByteArray safetyQrPayload(const QByteArray &fingerprintA, const QByteArray &fingerprintB);

/// Own long-term identity: generated once, secret key stored encrypted at
/// rest (Argon2id-derived AES-256-GCM) in the client database, unlocked for
/// the session on demand.
class FileTransferIdentity {
public:
	/// Ensure the tables exist (idempotent).
	static bool ensureSchema(QSqlDatabase &db);

	explicit FileTransferIdentity(QSqlDatabase &db);

	/// True when an identity row exists.
	bool hasIdentity() const;

	/// Generate a fresh identity keypair and store it encrypted under
	/// `passphrase`. Fails (returns false) if an identity already exists or
	/// the passphrase is too weak (< MinPasswordLength).
	bool createIdentity(const QString &passphrase);

	/// Load and decrypt the secret key. False on wrong passphrase (or
	/// corruption — indistinguishable by design).
	bool unlock(const QString &passphrase);

	/// True while the secret key is decrypted in memory.
	bool isUnlocked() const;

	/// Zeroize and forget the secret key.
	void lock();

	QByteArray publicKey() const;
	QByteArray fingerprint() const;

	/// Sign with the unlocked identity key (context-separated ML-DSA).
	bool sign(QByteArray &signature, const QByteArray &message, const QByteArray &context) const;

	/// Change the passphrase of the stored identity (requires unlock first).
	bool changePassphrase(const QString &newPassphrase);

private:
	bool loadRow(QByteArray &pubkey, QByteArray &seckeyEnc, QByteArray &salt, QByteArray &nonce,
				 Argon2Params &params) const;

	QSqlDatabase m_db;
	QByteArray m_publicKey;
	SecureBytes m_secretKey;
	bool m_unlocked = false;
};

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_FTIDENTITY_H_
