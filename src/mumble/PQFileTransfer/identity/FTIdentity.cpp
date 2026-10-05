// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/identity/FTIdentity.h"

#include "PQFileTransfer/crypto/Argon2Wrap.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cstring>

namespace PQFT {
namespace {

bool execIdentityQuery(QSqlQuery &query, const QString &statement = QString()) {
	const bool ok = statement.isEmpty() ? query.exec() : query.exec(statement);
	if (!ok) {
		qWarning("FileTransferIdentity: query failed: %s",
				 qPrintable(query.lastError().text()));
		return false;
	}
	return true;
}

QByteArray uint16be(quint16 value) {
	QByteArray out(2, Qt::Uninitialized);
	out[0] = static_cast< char >((value >> 8) & 0xff);
	out[1] = static_cast< char >(value & 0xff);
	return out;
}

QString argon2ParamsString(const Argon2Params &params) {
	return QString::fromLatin1("%1;%2;%3").arg(params.timeCost).arg(params.memoryKiB).arg(
		params.parallelism);
}

Argon2Params argon2ParamsFromString(const QString &string) {
	const QStringList parts = string.split(QLatin1Char(';'));
	Argon2Params params;
	if (parts.size() == 3) {
		params.timeCost    = parts.at(0).toUInt();
		params.memoryKiB   = parts.at(1).toUInt();
		params.parallelism = parts.at(2).toUInt();
	}
	return params;
}

} // namespace

QByteArray identityFingerprint(const QByteArray &mldsaPublicKey) {
	return sha384({ QByteArray(LabelFingerprint), QByteArray(SuiteId), mldsaPublicKey });
}

QByteArray safetyNumberHash(const QByteArray &fingerprintA, const QByteArray &fingerprintB) {
	if (fingerprintA.size() != HashSize || fingerprintB.size() != HashSize)
		return QByteArray();

	const QByteArray &minFp = fingerprintA < fingerprintB ? fingerprintA : fingerprintB;
	const QByteArray &maxFp = fingerprintA < fingerprintB ? fingerprintB : fingerprintA;

	const QByteArray lenMin = uint16be(static_cast< quint16 >(minFp.size()));
	const QByteArray lenMax = uint16be(static_cast< quint16 >(maxFp.size()));

	return sha384({ QByteArray(LabelSafetyNumber), QByteArray(SuiteId), lenMin, minFp, lenMax,
					maxFp });
}

QString formatSafetyNumberDigits(const QByteArray &safetyHash) {
	if (safetyHash.size() < 24)
		return QString();

	// First 24 bytes as a big-endian integer rendered in exactly 60 decimal
	// digits (zero-padded), grouped in fives.
	const int digits = 60;
	unsigned char value[24];
	memcpy(value, safetyHash.constData(), 24);

	QByteArray result(digits, '0');
	for (int digit = digits - 1; digit >= 0; --digit) {
		// Divide the 24-byte big-endian number by 10; remainder is the next digit
		unsigned int remainder = 0;
		for (int i = 0; i < 24; ++i) {
			const unsigned int current = remainder * 256 + value[i];
			value[i]					= static_cast< unsigned char >(current / 10);
			remainder					= current % 10;
		}
		result[digit] = static_cast< char >('0' + remainder);
	}

	QString grouped;
	grouped.reserve(digits + 11);
	for (int i = 0; i < digits; i += 5) {
		if (i > 0)
			grouped += QLatin1Char(' ');
		grouped += QString::fromLatin1(result.constData() + i, 5);
	}
	return grouped;
}

QString safetyNumber(const QByteArray &fingerprintA, const QByteArray &fingerprintB) {
	const QByteArray hash = safetyNumberHash(fingerprintA, fingerprintB);
	return formatSafetyNumberDigits(hash);
}

QByteArray safetyQrPayload(const QByteArray &fingerprintA, const QByteArray &fingerprintB) {
	const QByteArray hash = safetyNumberHash(fingerprintA, fingerprintB);
	if (hash.isEmpty())
		return QByteArray();

	const QByteArray &minFp = fingerprintA < fingerprintB ? fingerprintA : fingerprintB;
	const QByteArray &maxFp = fingerprintA < fingerprintB ? fingerprintB : fingerprintA;

	QVector< QPair< QCborValue, QCborValue > > entries;
	entries.append({ QCborValue(1), QCborValue(Version) });
	entries.append({ QCborValue(2), QCborValue(QString::fromLatin1(SuiteId)) });
	entries.append({ QCborValue(3), QCborValue(minFp) });
	entries.append({ QCborValue(4), QCborValue(maxFp) });
	entries.append({ QCborValue(5), QCborValue(hash) });
	return encodeCanonicalMap(entries);
}

// --- FileTransferIdentity ---------------------------------------------------------

bool FileTransferIdentity::ensureSchema(QSqlDatabase db) {
	QSqlQuery query(db);
	if (!execIdentityQuery(query, QLatin1String("CREATE TABLE IF NOT EXISTS `ft_identity` ("
								   "`id` INTEGER PRIMARY KEY, `pubkey` BLOB, `seckey_enc` BLOB, "
								   "`kek_salt` BLOB, `kek_nonce` BLOB, `kdf_params` TEXT, "
								   "`created` DATE)")))
		return false;
	return execIdentityQuery(query, QLatin1String("CREATE TABLE IF NOT EXISTS `ft_pins` ("
									  "`id` INTEGER PRIMARY KEY AUTOINCREMENT, "
									  "`server_digest` BLOB, `username` TEXT, "
									  "`peer_fingerprint` BLOB, `safety_number` TEXT, "
									  "`first_seen` DATE, `verified` INTEGER DEFAULT 0, "
									  "UNIQUE(`server_digest`, `username`))"));
}

FileTransferIdentity::FileTransferIdentity(QSqlDatabase db) : m_db(db) { }

bool FileTransferIdentity::hasIdentity() const {
	QSqlQuery query(m_db);
	if (!execIdentityQuery(query, QLatin1String("SELECT COUNT(*) FROM `ft_identity`")))
		return false;
	return query.next() && query.value(0).toInt() > 0;
}

bool FileTransferIdentity::createIdentity(const QString &passphrase) {
	if (passphrase.size() < MinPasswordLength)
		return false;
	if (hasIdentity())
		return false;

	SigMLDSA65 sig;
	QByteArray publicKey;
	SecureBytes secretKey;
	if (!sig.isValid() || !sig.keypair(publicKey, secretKey))
		return false;

	const QByteArray salt  = randomBytes(Argon2SaltSize);
	const QByteArray nonce = randomBytes(NonceSize);
	const Argon2Params params;

	QByteArray passphraseBytes = passphrase.toUtf8();
	SecureBytes kek;
	if (!argon2idDerive(kek, passphraseBytes, salt, params)) {
		return false;
	}

	QByteArray wrapped;
	if (!aesGcmEncrypt(wrapped, kek.toByteArray(), nonce, secretKey.toByteArray(),
					   identityFingerprint(publicKey))) {
		return false;
	}

	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("INSERT INTO `ft_identity` "
									 "(`id`, `pubkey`, `seckey_enc`, `kek_salt`, `kek_nonce`, "
									 "`kdf_params`, `created`) VALUES (1, ?, ?, ?, ?, ?, ?)")))
		return false;
	query.addBindValue(publicKey);
	query.addBindValue(wrapped);
	query.addBindValue(salt);
	query.addBindValue(nonce);
	query.addBindValue(argon2ParamsString(params));
	query.addBindValue(QDate::currentDate());
	if (!execIdentityQuery(query))
		return false;

	m_publicKey = publicKey;
	m_secretKey = std::move(secretKey);
	m_unlocked  = true;
	return true;
}

bool FileTransferIdentity::loadRow(QByteArray &pubkey, QByteArray &seckeyEnc, QByteArray &salt,
								   QByteArray &nonce, Argon2Params &params) const {
	QSqlQuery query(m_db);
	if (!execIdentityQuery(query, QLatin1String("SELECT `pubkey`, `seckey_enc`, `kek_salt`, `kek_nonce`, "
								   "`kdf_params` FROM `ft_identity` WHERE `id` = 1")))
		return false;
	if (!query.next())
		return false;

	pubkey	 = query.value(0).toByteArray();
	seckeyEnc = query.value(1).toByteArray();
	salt	  = query.value(2).toByteArray();
	nonce	  = query.value(3).toByteArray();
	params	  = argon2ParamsFromString(query.value(4).toString());

	return pubkey.size() == SigMLDSA65::PublicKeySize && seckeyEnc.size() == SigMLDSA65::SecretKeySize + TagSize
		   && salt.size() == Argon2SaltSize && nonce.size() == NonceSize && params.withinBounds();
}

bool FileTransferIdentity::unlock(const QString &passphrase) {
	lock();
	if (passphrase.isEmpty())
		return false;

	QByteArray pubkey, seckeyEnc, salt, nonce;
	Argon2Params params;
	if (!loadRow(pubkey, seckeyEnc, salt, nonce, params))
		return false;

	QByteArray passphraseBytes = passphrase.toUtf8();
	SecureBytes kek;
	if (!argon2idDerive(kek, passphraseBytes, salt, params)) {
		return false;
	}

	QByteArray plaintext;
	if (!aesGcmDecrypt(plaintext, kek.toByteArray(), nonce, seckeyEnc,
					   identityFingerprint(pubkey))) {
		return false;
	}
	if (plaintext.size() != SigMLDSA65::SecretKeySize) {
		zeroize(plaintext);
		return false;
	}

	m_publicKey = pubkey;
	m_secretKey = SecureBytes::fromByteArray(plaintext);
	m_unlocked  = true;
	return true;
}

void FileTransferIdentity::lock() {
	m_secretKey.clear();
	m_publicKey.clear();
	m_unlocked = false;
}

bool FileTransferIdentity::isUnlocked() const {
	return m_unlocked;
}

QByteArray FileTransferIdentity::publicKey() const {
	return m_unlocked ? m_publicKey : QByteArray();
}

QByteArray FileTransferIdentity::fingerprint() const {
	const QByteArray pk = m_unlocked ? m_publicKey : QByteArray();
	return pk.isEmpty() ? QByteArray() : identityFingerprint(pk);
}

bool FileTransferIdentity::sign(QByteArray &signature, const QByteArray &message,
								const QByteArray &context) const {
	if (!m_unlocked)
		return false;
	SigMLDSA65 sig;
	return sig.isValid() && sig.sign(signature, m_secretKey, message, context);
}

bool FileTransferIdentity::changePassphrase(const QString &newPassphrase) {
	if (!m_unlocked || newPassphrase.size() < MinPasswordLength)
		return false;

	const QByteArray salt  = randomBytes(Argon2SaltSize);
	const QByteArray nonce = randomBytes(NonceSize);
	const Argon2Params params;

	QByteArray passphraseBytes = newPassphrase.toUtf8();
	SecureBytes kek;
	if (!argon2idDerive(kek, passphraseBytes, salt, params)) {
		return false;
	}

	QByteArray wrapped;
	if (!aesGcmEncrypt(wrapped, kek.toByteArray(), nonce, m_secretKey.toByteArray(),
					   identityFingerprint(m_publicKey))) {
		return false;
	}

	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("UPDATE `ft_identity` SET `seckey_enc` = ?, `kek_salt` = ?, "
									 "`kek_nonce` = ?, `kdf_params` = ? WHERE `id` = 1")))
		return false;
	query.addBindValue(wrapped);
	query.addBindValue(salt);
	query.addBindValue(nonce);
	query.addBindValue(argon2ParamsString(params));
	return execIdentityQuery(query);
}

} // namespace PQFT
