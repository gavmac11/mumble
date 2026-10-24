// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QTest>

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/identity/FTIdentity.h"
#include "PQFileTransfer/identity/PeerTrustStore.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <QCborMap>
#include <QCborValue>

class TestFTIdentity : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void cleanupTestCase();

	void safetyNumberFormat();
	void safetyNumberSymmetry();
	void safetyNumberDistinct();
	void qrPayload();

	void identityLifecycle();
	void identityWrongPassphrase();
	void identityChangePassphrase();
	void identitySwitchesProtection();
	void deviceIdentityLifecycle();

	void trustStoreTransitions();
	void concurrentPinPreservesIdentity_data();
	void concurrentPinPreservesIdentity();
	void trustStoreChangedBlocks();
	void trustStoreRemovePin();
	void verificationMatchesExpectedKey();
	void replacedPinDoesNotAcceptOldVerification();

private:
	QSqlDatabase m_db;
	QTemporaryDir m_tempDir;
};

void TestFTIdentity::initTestCase() {
	QVERIFY(m_tempDir.isValid());
	const QString path = m_tempDir.filePath("identity.sqlite");
	m_db				  = QSqlDatabase::addDatabase("QSQLITE", "ft-identity-test");
	m_db.setDatabaseName(path);
	QVERIFY(m_db.open());
	QVERIFY(PQFT::FileTransferIdentity::ensureSchema(m_db));
}

void TestFTIdentity::cleanupTestCase() {
	m_db.close();
	m_db = QSqlDatabase();
	QSqlDatabase::removeDatabase("ft-identity-test");
}

void TestFTIdentity::safetyNumberFormat() {
	// 24 zero bytes -> exactly sixty zeros in twelve groups of five
	const QByteArray zeros(24, '\0');
	const QString formatted = PQFT::formatSafetyNumberDigits(zeros);
	QCOMPARE(formatted, QString("00000 00000 00000 00000 00000 00000 00000 00000 00000 00000 "
								"00000 00000"));

	// Value 1 (big-endian) -> zero-padded to 60 digits
	QByteArray one(24, '\0');
	one[23] = 0x01;
	QCOMPARE(PQFT::formatSafetyNumberDigits(one),
			 QString("00000 00000 00000 00000 00000 00000 00000 00000 00000 00000 00000 00001"));

	// Maximum value (2^192 - 1) renders zero-padded in 60 digits
	QByteArray max(24, '\xff');
	QCOMPARE(PQFT::formatSafetyNumberDigits(max),
			 QString("00627 71017 35386 68076 38357 89423 20766 64161 02355 44446 40345 12895"));
}

void TestFTIdentity::safetyNumberSymmetry() {
	const QByteArray fpA = PQFT::randomBytes(48);
	const QByteArray fpB = PQFT::randomBytes(48);
	QCOMPARE(PQFT::safetyNumber(fpA, fpB), PQFT::safetyNumber(fpB, fpA));
	QCOMPARE(PQFT::safetyNumberHash(fpA, fpB), PQFT::safetyNumberHash(fpB, fpA));
	QCOMPARE(PQFT::safetyNumber(fpA, fpA).isEmpty(), false);

	// Deterministic
	QCOMPARE(PQFT::safetyNumber(fpA, fpB), PQFT::safetyNumber(fpA, fpB));

	// Wrong-size fingerprints produce no hash
	QVERIFY(PQFT::safetyNumberHash(fpA.left(10), fpB).isEmpty());
}

void TestFTIdentity::safetyNumberDistinct() {
	const QByteArray fpA = PQFT::randomBytes(48);
	QVERIFY(PQFT::safetyNumber(fpA, PQFT::randomBytes(48)) != PQFT::safetyNumber(fpA, PQFT::randomBytes(48)));
}

void TestFTIdentity::qrPayload() {
	const QByteArray fpA = PQFT::randomBytes(48);
	const QByteArray fpB = PQFT::randomBytes(48);
	const QByteArray payload = PQFT::safetyQrPayload(fpA, fpB);
	QVERIFY(!payload.isEmpty());

	// Must be canonical CBOR and carry the full hash
	QCborValue value;
	QVERIFY(PQFT::decodeCanonical(payload, value));
	const QCborMap map = value.toMap();
	QCOMPARE(map.value(QCborValue(1)).toInteger(), 1);
	QCOMPARE(map.value(QCborValue(2)).toString(), QString(PQFT::SuiteId));
	QCOMPARE(map.value(QCborValue(5)).toByteArray(), PQFT::safetyNumberHash(fpA, fpB));
}

void TestFTIdentity::identityLifecycle() {
	PQFT::FileTransferIdentity identity(m_db);
	QVERIFY(!identity.hasIdentity());
	QVERIFY(!identity.isUnlocked());

	// Weak passphrase is rejected
	QVERIFY(!identity.createIdentity(QString("short")));

	QVERIFY(identity.createIdentity(QString("correct horse battery staple")));
	QVERIFY(identity.hasIdentity());
	QVERIFY(identity.isUnlocked());

	const QByteArray pk = identity.publicKey();
	QCOMPARE(pk.size(), PQFT::SigMLDSA65::PublicKeySize);
	QCOMPARE(identity.fingerprint(), PQFT::identityFingerprint(pk));

	// Sign / verify round trip with context separation
	QByteArray signature;
	const QByteArray message = PQFT::randomBytes(100);
	QVERIFY(identity.sign(signature, message, QByteArray(PQFT::LabelManifestCtx)));

	PQFT::SigMLDSA65 sig;
	QVERIFY(sig.verify(pk, message, signature, QByteArray(PQFT::LabelManifestCtx)));
	QVERIFY(!sig.verify(pk, message, signature, QByteArray(PQFT::LabelHandshakeCtx)));

	// Second createIdentity is refused (one identity per database)
	QVERIFY(!identity.createIdentity(QString("another passphrase entirely")));

	// Lock clears everything
	identity.lock();
	QVERIFY(!identity.isUnlocked());
	QVERIFY(identity.publicKey().isEmpty());
	QVERIFY(!identity.sign(signature, message, QByteArray(PQFT::LabelManifestCtx)));
}

void TestFTIdentity::identityWrongPassphrase() {
	PQFT::FileTransferIdentity identity(m_db);
	QVERIFY(identity.unlock(QString("correct horse battery staple")));
	identity.lock();

	QVERIFY(!identity.unlock(QString("totally wrong passphrase")));
	QVERIFY(!identity.isUnlocked());

	// Correct one still works
	QVERIFY(identity.unlock(QString("correct horse battery staple")));
	QVERIFY(identity.isUnlocked());
}

void TestFTIdentity::identityChangePassphrase() {
	PQFT::FileTransferIdentity identity(m_db);
	QVERIFY(identity.unlock(QString("correct horse battery staple")));

	QVERIFY(identity.changePassphrase(QString("new passphrase even better")));
	identity.lock();

	QVERIFY(!identity.unlock(QString("correct horse battery staple")));
	QVERIFY(identity.unlock(QString("new passphrase even better")));

	// The key itself survived the re-wrap
	QCOMPARE(identity.publicKey().size(), PQFT::SigMLDSA65::PublicKeySize);
}

void TestFTIdentity::identitySwitchesProtection() {
	PQFT::FileTransferIdentity identity(m_db);
	QCOMPARE(identity.protection(), PQFT::FileTransferIdentity::Protection::Passphrase);
	QVERIFY(!identity.unlockWithDevice());
	QVERIFY(identity.unlock(QString("new passphrase even better")));
	const QByteArray fingerprint = identity.fingerprint();

	// Removing the passphrase keeps the key pair and unlocks without input.
	QVERIFY(identity.removePassphrase());
	QCOMPARE(identity.protection(), PQFT::FileTransferIdentity::Protection::Device);
	identity.lock();
	QVERIFY(!identity.unlock(QString("new passphrase even better")));
	QVERIFY(identity.unlockWithDevice());
	QCOMPARE(identity.fingerprint(), fingerprint);

	// A passphrase can be chosen again later.
	QVERIFY(!identity.changePassphrase(QString("short")));
	QVERIFY(identity.changePassphrase(QString("back to a passphrase")));
	QCOMPARE(identity.protection(), PQFT::FileTransferIdentity::Protection::Passphrase);
	identity.lock();
	QVERIFY(!identity.unlockWithDevice());
	QVERIFY(!identity.removePassphrase()); // requires an unlocked identity
	QVERIFY(identity.unlock(QString("back to a passphrase")));
	QCOMPARE(identity.fingerprint(), fingerprint);
}

void TestFTIdentity::deviceIdentityLifecycle() {
	const QString connection = QStringLiteral("ft-device-identity-test");
	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
		db.setDatabaseName(m_tempDir.filePath("device-identity.sqlite"));
		QVERIFY(db.open());
		QVERIFY(PQFT::FileTransferIdentity::ensureSchema(db));

		PQFT::FileTransferIdentity identity(db);
		QCOMPARE(identity.protection(), PQFT::FileTransferIdentity::Protection::None);
		QVERIFY(!identity.unlockWithDevice());
		QVERIFY(identity.createDeviceIdentity());
		QVERIFY(identity.isUnlocked());
		QCOMPARE(identity.protection(), PQFT::FileTransferIdentity::Protection::Device);
		const QByteArray fingerprint = identity.fingerprint();
		QCOMPARE(fingerprint.size(), PQFT::HashSize);
		// One identity per database, whatever the protection.
		QVERIFY(!identity.createDeviceIdentity());
		QVERIFY(!identity.createIdentity(QString("another passphrase entirely")));

		// A fresh instance (next launch) unlocks silently with the same key.
		PQFT::FileTransferIdentity relaunched(db);
		QVERIFY(!relaunched.isUnlocked());
		QVERIFY(!relaunched.unlock(QString("any passphrase at all")));
		QVERIFY(relaunched.unlockWithDevice());
		QCOMPARE(relaunched.fingerprint(), fingerprint);

		QByteArray signature;
		const QByteArray message = PQFT::randomBytes(64);
		QVERIFY(relaunched.sign(signature, message, QByteArray(PQFT::LabelManifestCtx)));
		PQFT::SigMLDSA65 sig;
		QVERIFY(sig.verify(relaunched.publicKey(), message, signature, QByteArray(PQFT::LabelManifestCtx)));

		// Starting over removes the identity; a new one has a new key.
		QVERIFY(relaunched.deleteIdentity());
		QVERIFY(!relaunched.isUnlocked());
		QVERIFY(!relaunched.hasIdentity());
		QCOMPARE(relaunched.protection(), PQFT::FileTransferIdentity::Protection::None);
		QVERIFY(relaunched.createDeviceIdentity());
		QVERIFY(relaunched.fingerprint() != fingerprint);
		db.close();
	}
	QSqlDatabase::removeDatabase(connection);
}

void TestFTIdentity::concurrentPinPreservesIdentity_data() {
	QTest::addColumn< bool >("sameKey");
	QTest::newRow("same-key") << true;
	QTest::newRow("different-key") << false;
}

void TestFTIdentity::concurrentPinPreservesIdentity() {
	QFETCH(bool, sameKey);
	PQFT::PeerTrustStore store(m_db);
	const auto digest    = PQFT::randomBytes(20);
	const auto fp        = PQFT::randomBytes(PQFT::HashSize);
	const auto competing = sameKey ? fp : PQFT::randomBytes(PQFT::HashSize);
	QSqlQuery query(m_db);
	// Insert between the initial check and INSERT without a timing-dependent race.
	const QString trigger =
		QStringLiteral(
			"CREATE TEMP TRIGGER competing_pin BEFORE INSERT ON ft_pins "
			"WHEN NOT EXISTS (SELECT 1 FROM ft_pins WHERE server_digest=NEW.server_digest AND username=NEW.username) "
			"BEGIN INSERT INTO ft_pins(server_digest,username,peer_fingerprint,safety_number,first_seen,verified) "
			"VALUES(NEW.server_digest,NEW.username,X'%1','earlier','2000-01-01',1); END")
			.arg(QString::fromLatin1(competing.toHex()));
	QVERIFY(query.exec(trigger));
	const auto state = store.checkAndPin(digest, "Concurrent", fp, "shown");
	QVERIFY(query.exec("DROP TRIGGER competing_pin"));
	QCOMPARE(state, sameKey ? PQFT::TrustState::Verified : PQFT::TrustState::Changed);
	PQFT::PinnedPeer peer;
	QVERIFY(store.lookup(peer, digest, "Concurrent"));
	QCOMPARE(peer.fingerprint, competing);
	QVERIFY(peer.verified);
	QCOMPARE(peer.firstSeen, QDate(2000, 1, 1));
	QCOMPARE(store.checkAndPin(digest, "Concurrent", fp, "shown"), state);
}

void TestFTIdentity::trustStoreTransitions() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray serverDigest = PQFT::randomBytes(48);
	const QString username("alice");
	const QByteArray fp = PQFT::randomBytes(48);
	const QString safety = PQFT::safetyNumber(fp, fp); // any display string

	QCOMPARE(store.check(serverDigest, username, fp, safety), PQFT::TrustState::NewPeer);

	// TOFU: first contact pins
	QCOMPARE(store.checkAndPin(serverDigest, username, fp, safety), PQFT::TrustState::Pinned);
	QCOMPARE(store.check(serverDigest, username, fp, safety), PQFT::TrustState::Pinned);

	// Same fingerprint from a different username is a separate pin
	QCOMPARE(store.check(serverDigest, "bob", fp, safety), PQFT::TrustState::NewPeer);

	// Verification upgrades the state
	QVERIFY(store.markVerified(serverDigest, username, fp));
	QCOMPARE(store.check(serverDigest, username, fp, safety), PQFT::TrustState::Verified);

	PQFT::PinnedPeer peer;
	QVERIFY(store.lookup(peer, serverDigest, username));
	QCOMPARE(peer.fingerprint, fp);
	QCOMPARE(peer.username, username);
	QCOMPARE(peer.verified, true);
}

void TestFTIdentity::trustStoreChangedBlocks() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray serverDigest = PQFT::randomBytes(48);
	const QString username("carol");
	const QByteArray fp = PQFT::randomBytes(48);
	const QString safety("00000 00000 00000 00000 00000 00000 00000 00000 00000 00000 00000 00000");

	QCOMPARE(store.checkAndPin(serverDigest, username, fp, safety), PQFT::TrustState::Pinned);

	const QByteArray attackerFp = PQFT::randomBytes(48);
	QCOMPARE(store.check(serverDigest, username, attackerFp, safety), PQFT::TrustState::Changed);
	QCOMPARE(store.checkAndPin(serverDigest, username, attackerFp, safety), PQFT::TrustState::Changed);

	// The original pin is untouched (no silent key updates, ever)
	PQFT::PinnedPeer peer;
	QVERIFY(store.lookup(peer, serverDigest, username));
	QCOMPARE(peer.fingerprint, fp);
}

void TestFTIdentity::verificationMatchesExpectedKey() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray digest = PQFT::randomBytes(20), shown = PQFT::randomBytes(PQFT::HashSize),
					 other = PQFT::randomBytes(PQFT::HashSize);
	const QString name     = QStringLiteral("Expected-key fixture");
	QVERIFY(!store.markVerified(digest, name, shown));
	QCOMPARE(store.checkAndPin(digest, name, shown, QStringLiteral("shown safety")), PQFT::TrustState::Pinned);
	QVERIFY(!store.markVerified(digest, name, other));
	PQFT::PinnedPeer stored;
	QVERIFY(store.lookup(stored, digest, name));
	QCOMPARE(stored.fingerprint, shown);
	QVERIFY(!stored.verified);
	QVERIFY(store.markVerified(digest, name, shown));
	QCOMPARE(store.check(digest, name, shown, QString()), PQFT::TrustState::Verified);
}

void TestFTIdentity::replacedPinDoesNotAcceptOldVerification() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray digest = PQFT::randomBytes(20), old = PQFT::randomBytes(PQFT::HashSize),
					 replacement = PQFT::randomBytes(PQFT::HashSize);
	const QString name           = QStringLiteral("Replacement-key fixture");
	QCOMPARE(store.checkAndPin(digest, name, old, QString()), PQFT::TrustState::Pinned);
	QVERIFY(store.removePin(digest, name, old));
	QCOMPARE(store.checkAndPin(digest, name, replacement, QString()), PQFT::TrustState::Pinned);
	QVERIFY(!store.markVerified(digest, name, old));
	QVERIFY(!store.removePin(digest, name, old));
	PQFT::PinnedPeer stored;
	QVERIFY(store.lookup(stored, digest, name));
	QCOMPARE(stored.fingerprint, replacement);
	QVERIFY(!stored.verified);
}

void TestFTIdentity::trustStoreRemovePin() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray serverDigest = PQFT::randomBytes(48);
	const QString username("dave");
	const QByteArray fp = PQFT::randomBytes(48);
	const QString safety("x");

	QCOMPARE(store.checkAndPin(serverDigest, username, fp, safety), PQFT::TrustState::Pinned);
	QVERIFY(store.removePin(serverDigest, username, fp));
	QCOMPARE(store.check(serverDigest, username, fp, safety), PQFT::TrustState::NewPeer);
}

QTEST_MAIN(TestFTIdentity)
#include "TestFTIdentity.moc"
