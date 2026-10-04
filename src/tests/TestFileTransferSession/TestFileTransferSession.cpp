// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QTest>

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FTMessages.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "PQFileTransfer/identity/FTIdentity.h"

namespace {
struct TestIdentity {
	PQFT::SessionIdentity toSessionIdentity() const {
		PQFT::SessionIdentity identity;
		identity.publicKey = publicKey;
		// The TestIdentity outlives every session using it; capture by reference.
		identity.sign		 = [&secret = secretKey](QByteArray &signature, const QByteArray &message,
									 const QByteArray &context) {
			PQFT::SigMLDSA65 sig;
			return sig.sign(signature, secret, message, context);
		 };
		return identity;
	}

	QByteArray publicKey;
	PQFT::SecureBytes secretKey;
};

TestIdentity generateIdentity() {
	TestIdentity identity;
	PQFT::SigMLDSA65 sig;
	sig.keypair(identity.publicKey, identity.secretKey);
	return identity;
}
} // namespace

class TestFileTransferSession : public QObject {
	Q_OBJECT
private slots:
	void happyPathDerivesSameKeys();
	void wrongPinFails();
	void tamperedHandshakeFails();
	void replayedFrameFails();
	void controlChannelSequencing();
	void sessionKekBoundToTransfer();
	void controlTamperFails();

private:
	// Wire a fresh initiator/responder pair through the full handshake.
	bool runHandshake(PQFT::FileTransferSession &initiator, PQFT::FileTransferSession &responder);
};

bool TestFileTransferSession::runHandshake(PQFT::FileTransferSession &initiator,
										  PQFT::FileTransferSession &responder) {
	if (initiator.state() != PQFT::FileTransferSession::State::Created)
		return false;
	const QByteArray m1 = initiator.buildM1();
	const QByteArray m2 = responder.processM1(m1) ? responder.buildM2() : QByteArray();
	if (m2.isEmpty() || !initiator.processM2(m2))
		return false;
	const QByteArray m3 = initiator.buildM3();
	const QByteArray m4 = (responder.processM3(m3)) ? responder.buildM4() : QByteArray();
	if (m4.isEmpty() || !initiator.processM4(m4))
		return false;
	return initiator.established() && responder.established();
}

void TestFileTransferSession::happyPathDerivesSameKeys() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	const QByteArray fpA = PQFT::identityFingerprint(idA.publicKey);
	const QByteArray fpB = PQFT::identityFingerprint(idB.publicKey);

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(), fpB);
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(), fpA);
	QVERIFY(runHandshake(initiator, responder));

	// Both sides derive the identical PRK (exported for tests)
	QCOMPARE(initiator.debugPrk(), responder.debugPrk());

	// And identical per-transfer session KEKs
	const QByteArray transferId = PQFT::randomBytes(PQFT::TransferIdSize);
	QCOMPARE(initiator.deriveSessionKek(transferId), responder.deriveSessionKek(transferId));
	QCOMPARE(initiator.deriveWrapNonce1(transferId), responder.deriveWrapNonce1(transferId));
	QCOMPARE(initiator.deriveWrapNonce1(transferId).size(), PQFT::NonceSize);

	// Control channel round trips in both directions (bodies are canonical CBOR)
	QVector< QPair< QCborValue, QCborValue > > bodyEntries;
	bodyEntries.append({ QCborValue(1), QCborValue(PQFT::randomBytes(64)) });
	const QByteArray body = PQFT::encodeCanonicalMap(bodyEntries);
	QVERIFY(!body.isEmpty());
	const QByteArray sealed = initiator.sealControl(PQFT::FTFrame::TypeManifest, body);
	QVERIFY(!sealed.isEmpty());

	quint8 type	 = 0;
	QByteArray opened;
	QVERIFY(responder.openControl(type, opened, sealed));
	QCOMPARE(type, static_cast< quint8 >(PQFT::FTFrame::TypeManifest));
	QCOMPARE(opened, body);
}

void TestFileTransferSession::wrongPinFails() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();
	TestIdentity idAttacker = generateIdentity(); // substitute identity

	// Responder pinned the attacker's fingerprint instead of A's
	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idAttacker.publicKey));

	const QByteArray m1 = initiator.buildM1();
	QVERIFY(!m1.isEmpty());
	QVERIFY(!responder.processM1(m1));
	QCOMPARE(responder.state(), PQFT::FileTransferSession::State::Failed);
}

void TestFileTransferSession::tamperedHandshakeFails() {
	// Signature-covered transcript bit flip in M2
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idA.publicKey));

	QByteArray m1 = initiator.buildM1();
	QVERIFY(responder.processM1(m1));
	QByteArray m2 = responder.buildM2();
	QVERIFY(initiator.processM2(m2));

	QByteArray m3 = initiator.buildM3();
	// Flip a byte inside the ML-KEM ciphertext region of the body
	m3[m3.size() / 2] = m3[m3.size() / 2] ^ 0x01;
	QVERIFY(!responder.processM3(m3));
	QCOMPARE(responder.state(), PQFT::FileTransferSession::State::Failed);

	// Tampered MAC_B
	PQFT::FileTransferSession init2(PQFT::FileTransferSession::Role::Initiator,
									idA.toSessionIdentity(), PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession resp2(PQFT::FileTransferSession::Role::Responder,
									idB.toSessionIdentity(), PQFT::identityFingerprint(idA.publicKey));
	QByteArray m1b = init2.buildM1();
	QVERIFY(resp2.processM1(m1b));
	QVERIFY(init2.processM2(resp2.buildM2()));
	QVERIFY(resp2.processM3(init2.buildM3()));
	QByteArray m4 = resp2.buildM4();
	m4[m4.size() - 1] = m4[m4.size() - 1] ^ 0x01;
	QVERIFY(!init2.processM4(m4));
	QCOMPARE(init2.state(), PQFT::FileTransferSession::State::Failed);
}

void TestFileTransferSession::replayedFrameFails() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idA.publicKey));

	const QByteArray m1 = initiator.buildM1();
	QVERIFY(responder.processM1(m1));
	const QByteArray m2 = responder.buildM2();
	QVERIFY(initiator.processM2(m2));

	// Replaying M2 to the initiator again must not advance anything
	QVERIFY(!initiator.processM2(m2));
	// The session is still waiting for nothing — M3 cannot be built after failure
	// (state machine moved on), but let's assert the state explicitly.
	QVERIFY(initiator.state() != PQFT::FileTransferSession::State::Established);
}

void TestFileTransferSession::controlChannelSequencing() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idA.publicKey));
	QVERIFY(runHandshake(initiator, responder));

	const QByteArray frame0 = initiator.sealControl(PQFT::FTFrame::TypeManifest, QByteArray::fromHex("a10101")); // {1: 1}
	const QByteArray frame1 = initiator.sealControl(PQFT::FTFrame::TypeComplete, QByteArray::fromHex("a10102")); // {1: 2}
	QVERIFY(!frame0.isEmpty() && !frame1.isEmpty());

	// Out of order: frame1 before frame0 must fail
	quint8 type = 0;
	QByteArray body;
	QVERIFY(!responder.openControl(type, body, frame1));

	// Replay of frame0 after accepting frame0 must fail — but the failed
	// attempt above killed the session (strict sequencing is fatal), so use
	// a fresh pair for the replay case.
	PQFT::FileTransferSession initiator2(PQFT::FileTransferSession::Role::Initiator,
										 idA.toSessionIdentity(),
										 PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder2(PQFT::FileTransferSession::Role::Responder,
										 idB.toSessionIdentity(),
										 PQFT::identityFingerprint(idA.publicKey));
	QVERIFY(runHandshake(initiator2, responder2));
	const QByteArray r0 = initiator2.sealControl(PQFT::FTFrame::TypeManifest, QByteArray::fromHex("a10101")); // {1: 1}
	QVERIFY(responder2.openControl(type, body, r0));
	QVERIFY(!responder2.openControl(type, body, r0)); // duplicate is fatal
	QCOMPARE(responder2.state(), PQFT::FileTransferSession::State::Failed);
}

void TestFileTransferSession::sessionKekBoundToTransfer() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idA.publicKey));
	QVERIFY(runHandshake(initiator, responder));

	const QByteArray transfer1 = PQFT::randomBytes(PQFT::TransferIdSize);
	const QByteArray transfer2 = PQFT::randomBytes(PQFT::TransferIdSize);
	QVERIFY(initiator.deriveSessionKek(transfer1) != initiator.deriveSessionKek(transfer2));

	// Malformed transfer ids are rejected
	QVERIFY(initiator.deriveSessionKek(QByteArray(15, 'x')).isEmpty());
}

void TestFileTransferSession::controlTamperFails() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();

	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator,
										idA.toSessionIdentity(),
										PQFT::identityFingerprint(idB.publicKey));
	PQFT::FileTransferSession responder(PQFT::FileTransferSession::Role::Responder,
										idB.toSessionIdentity(),
										PQFT::identityFingerprint(idA.publicKey));
	QVERIFY(runHandshake(initiator, responder));

	QByteArray frame = initiator.sealControl(PQFT::FTFrame::TypeManifest, PQFT::randomBytes(100));
	frame[frame.size() - 3] = frame[frame.size() - 3] ^ 0x01; // inside the GCM tag

	quint8 type = 0;
	QByteArray body;
	QVERIFY(!responder.openControl(type, body, frame));
	QCOMPARE(responder.state(), PQFT::FileTransferSession::State::Failed);
}

QTEST_MAIN(TestFileTransferSession)
#include "TestFileTransferSession.moc"
