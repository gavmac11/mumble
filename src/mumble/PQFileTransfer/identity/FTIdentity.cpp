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

#ifdef Q_OS_WIN
#	include "win.h"

#	include <dpapi.h>
#	ifdef _MSC_VER
#		pragma comment(lib, "crypt32.lib")
#	endif
#endif

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

// kdf_params markers of device-protected identities. Passphrase identities
// store their Argon2id parameters ("t;m;p") in the same column.
const QLatin1String DeviceDpapiMarker("device-dpapi");
const QLatin1String DeviceLocalMarker("device-local");

bool isDeviceMarker(const QString &kdfParams) {
	return kdfParams == DeviceDpapiMarker || kdfParams == DeviceLocalMarker;
}

/// Seal the random device key for storage in the `kek_salt` column. On
/// Windows DPAPI binds it to the user account. Elsewhere it is stored as is,
/// which matches how Mumble stores its client certificate.
bool sealDeviceKey(QByteArray &sealed, QString &marker, const QByteArray &key) {
#ifdef Q_OS_WIN
	DATA_BLOB input;
	input.cbData     = static_cast< DWORD >(key.size());
	input.pbData     = reinterpret_cast< BYTE * >(const_cast< char * >(key.constData()));
	DATA_BLOB output = {};
	if (!CryptProtectData(&input, L"Mumble file-transfer identity", nullptr, nullptr, nullptr,
						  CRYPTPROTECT_UI_FORBIDDEN, &output))
		return false;
	sealed = QByteArray(reinterpret_cast< const char * >(output.pbData), static_cast< qsizetype >(output.cbData));
	LocalFree(output.pbData);
	marker = DeviceDpapiMarker;
	return !sealed.isEmpty();
#else
	sealed = key;
	marker = DeviceLocalMarker;
	return sealed.size() == KeySize;
#endif
}

bool openDeviceKey(SecureBytes &key, const QByteArray &sealed, const QString &marker) {
	if (marker == DeviceLocalMarker) {
		if (sealed.size() != KeySize)
			return false;
		key = SecureBytes::fromByteArray(sealed);
		return true;
	}
#ifdef Q_OS_WIN
	if (marker == DeviceDpapiMarker && !sealed.isEmpty()) {
		DATA_BLOB input;
		input.cbData     = static_cast< DWORD >(sealed.size());
		input.pbData     = reinterpret_cast< BYTE * >(const_cast< char * >(sealed.constData()));
		DATA_BLOB output = {};
		if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
			return false;
		QByteArray plain(reinterpret_cast< const char * >(output.pbData), static_cast< qsizetype >(output.cbData));
		SecureZeroMemory(output.pbData, output.cbData);
		LocalFree(output.pbData);
		const bool ok = plain.size() == KeySize;
		if (ok)
			key = SecureBytes::fromByteArray(plain);
		zeroize(plain);
		return ok;
	}
#endif
	return false;
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

FileTransferIdentity::FileTransferIdentity(QSqlDatabase db) : m_db(db) {
}

bool FileTransferIdentity::hasIdentity() const {
	QSqlQuery query(m_db);
	if (!execIdentityQuery(query, QLatin1String("SELECT COUNT(*) FROM `ft_identity`")))
		return false;
	return query.next() && query.value(0).toInt() > 0;
}

FileTransferIdentity::Protection FileTransferIdentity::protection() const {
	StoredRow row;
	if (!loadRow(row))
		return Protection::None;
	return isDeviceMarker(row.kdfParams) ? Protection::Device : Protection::Passphrase;
}

bool FileTransferIdentity::storeWrapped(bool insert, const QByteArray &pubkey, const QByteArray &wrapped,
										const QByteArray &kekSalt, const QByteArray &nonce, const QString &kdfParams) {
	QSqlQuery query(m_db);
	if (insert) {
		if (!query.prepare(QLatin1String("INSERT INTO `ft_identity` "
										 "(`id`, `pubkey`, `seckey_enc`, `kek_salt`, `kek_nonce`, "
										 "`kdf_params`, `created`) VALUES (1, ?, ?, ?, ?, ?, ?)")))
			return false;
		query.addBindValue(pubkey);
		query.addBindValue(wrapped);
		query.addBindValue(kekSalt);
		query.addBindValue(nonce);
		query.addBindValue(kdfParams);
		query.addBindValue(QDate::currentDate());
	} else {
		if (!query.prepare(QLatin1String("UPDATE `ft_identity` SET `seckey_enc` = ?, `kek_salt` = ?, "
										 "`kek_nonce` = ?, `kdf_params` = ? WHERE `id` = 1 AND `pubkey` = ?")))
			return false;
		query.addBindValue(wrapped);
		query.addBindValue(kekSalt);
		query.addBindValue(nonce);
		query.addBindValue(kdfParams);
		query.addBindValue(pubkey);
	}
	if (!execIdentityQuery(query))
		return false;
	return insert || query.numRowsAffected() == 1;
}

bool FileTransferIdentity::createDeviceIdentity() {
	if (hasIdentity())
		return false;

	SigMLDSA65 sig;
	QByteArray publicKey;
	SecureBytes secretKey;
	if (!sig.isValid() || !sig.keypair(publicKey, secretKey))
		return false;

	const SecureBytes kek = SecureBytes::random(KeySize);
	QByteArray sealed;
	QString marker;
	if (kek.size() != KeySize || !sealDeviceKey(sealed, marker, kek.toByteArray()))
		return false;

	const QByteArray nonce = randomBytes(NonceSize);
	QByteArray wrapped;
	if (!aesGcmEncrypt(wrapped, kek.toByteArray(), nonce, secretKey.toByteArray(), identityFingerprint(publicKey)))
		return false;
	if (!storeWrapped(true, publicKey, wrapped, sealed, nonce, marker))
		return false;

	m_publicKey = publicKey;
	m_secretKey = std::move(secretKey);
	m_unlocked  = true;
	return true;
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
	if (!aesGcmEncrypt(wrapped, kek.toByteArray(), nonce, secretKey.toByteArray(), identityFingerprint(publicKey))) {
		return false;
	}
	if (!storeWrapped(true, publicKey, wrapped, salt, nonce, argon2ParamsString(params)))
		return false;

	m_publicKey = publicKey;
	m_secretKey = std::move(secretKey);
	m_unlocked  = true;
	return true;
}

bool FileTransferIdentity::loadRow(StoredRow &row) const {
	QSqlQuery query(m_db);
	if (!execIdentityQuery(query, QLatin1String("SELECT `pubkey`, `seckey_enc`, `kek_salt`, `kek_nonce`, "
												"`kdf_params` FROM `ft_identity` WHERE `id` = 1")))
		return false;
	if (!query.next())
		return false;

	row.pubkey    = query.value(0).toByteArray();
	row.seckeyEnc = query.value(1).toByteArray();
	row.kekSalt   = query.value(2).toByteArray();
	row.nonce     = query.value(3).toByteArray();
	row.kdfParams = query.value(4).toString();

	return row.pubkey.size() == SigMLDSA65::PublicKeySize && row.seckeyEnc.size() == SigMLDSA65::SecretKeySize + TagSize
		   && row.nonce.size() == NonceSize;
}

bool FileTransferIdentity::finishUnlock(const StoredRow &row, const QByteArray &kek) {
	QByteArray plaintext;
	if (!aesGcmDecrypt(plaintext, kek, row.nonce, row.seckeyEnc, identityFingerprint(row.pubkey))) {
		return false;
	}
	if (plaintext.size() != SigMLDSA65::SecretKeySize) {
		zeroize(plaintext);
		return false;
	}

	m_publicKey = row.pubkey;
	m_secretKey = SecureBytes::fromByteArray(plaintext);
	zeroize(plaintext);
	m_unlocked = true;
	return true;
}

bool FileTransferIdentity::unlock(const QString &passphrase) {
	lock();
	if (passphrase.isEmpty())
		return false;

	StoredRow row;
	if (!loadRow(row) || isDeviceMarker(row.kdfParams))
		return false;
	const Argon2Params params = argon2ParamsFromString(row.kdfParams);
	if (row.kekSalt.size() != Argon2SaltSize || !params.withinBounds())
		return false;

	QByteArray passphraseBytes = passphrase.toUtf8();
	SecureBytes kek;
	if (!argon2idDerive(kek, passphraseBytes, row.kekSalt, params)) {
		return false;
	}
	return finishUnlock(row, kek.toByteArray());
}

bool FileTransferIdentity::unlockWithDevice() {
	lock();
	StoredRow row;
	if (!loadRow(row) || !isDeviceMarker(row.kdfParams))
		return false;
	SecureBytes kek;
	if (!openDeviceKey(kek, row.kekSalt, row.kdfParams))
		return false;
	return finishUnlock(row, kek.toByteArray());
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

bool FileTransferIdentity::sign(QByteArray &signature, const QByteArray &message, const QByteArray &context) const {
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
	return storeWrapped(false, m_publicKey, wrapped, salt, nonce, argon2ParamsString(params));
}

bool FileTransferIdentity::removePassphrase() {
	if (!m_unlocked)
		return false;

	const SecureBytes kek = SecureBytes::random(KeySize);
	QByteArray sealed;
	QString marker;
	if (kek.size() != KeySize || !sealDeviceKey(sealed, marker, kek.toByteArray()))
		return false;

	const QByteArray nonce = randomBytes(NonceSize);
	QByteArray wrapped;
	if (!aesGcmEncrypt(wrapped, kek.toByteArray(), nonce, m_secretKey.toByteArray(), identityFingerprint(m_publicKey)))
		return false;
	return storeWrapped(false, m_publicKey, wrapped, sealed, nonce, marker);
}

bool FileTransferIdentity::deleteIdentity() {
	lock();
	QSqlQuery query(m_db);
	return execIdentityQuery(query, QLatin1String("DELETE FROM `ft_identity`"));
}

} // namespace PQFT
