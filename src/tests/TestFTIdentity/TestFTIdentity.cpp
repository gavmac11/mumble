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

	void trustStoreTransitions();
	void trustStoreChangedBlocks();
	void trustStoreRemovePin();

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
	QVERIFY(PQFT::safetyNumber(fpA, PQFT::randomBytes(48))
			!= PQFT::safetyNumber(fpA, PQFT::randomBytes(48)));
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
	QVERIFY(store.markVerified(serverDigest, username));
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
	QCOMPARE(store.checkAndPin(serverDigest, username, attackerFp, safety),
			 PQFT::TrustState::Changed);

	// The original pin is untouched (no silent key updates, ever)
	PQFT::PinnedPeer peer;
	QVERIFY(store.lookup(peer, serverDigest, username));
	QCOMPARE(peer.fingerprint, fp);
}

void TestFTIdentity::trustStoreRemovePin() {
	PQFT::PeerTrustStore store(m_db);
	const QByteArray serverDigest = PQFT::randomBytes(48);
	const QString username("dave");
	const QByteArray fp = PQFT::randomBytes(48);
	const QString safety("x");

	QCOMPARE(store.checkAndPin(serverDigest, username, fp, safety), PQFT::TrustState::Pinned);
	QVERIFY(store.removePin(serverDigest, username));
	QCOMPARE(store.check(serverDigest, username, fp, safety), PQFT::TrustState::NewPeer);
}

QTEST_MAIN(TestFTIdentity)
#include "TestFTIdentity.moc"
