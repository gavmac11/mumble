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
#include "PQFileTransfer/crypto/Merkle.h"
#include "PQFileTransfer/engine/FTManifest.h"
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
	void manifestRoundTrip();
	void manifestTamperFails();
	void chunkRoundTrip();
	void fileKeyDoubleWrap();

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

// ---- Manifest + chunk encryption -------------------------------------------------

void TestFileTransferSession::manifestRoundTrip() {
	TestIdentity idA = generateIdentity();
	TestIdentity idB = generateIdentity();
	auto signA = [&idA](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
		PQFT::SigMLDSA65 s;
		return s.sign(sig, idA.secretKey, msg, ctx);
	};
	auto verifyA = [&idA](const QByteArray &msg, const QByteArray &sig, const QByteArray &ctx) {
		PQFT::SigMLDSA65 s;
		return s.verify(idA.publicKey, msg, sig, ctx);
	};

	PQFT::FTManifest m;
	m.transferId   = PQFT::randomBytes(PQFT::TransferIdSize);
	m.fpA		   = PQFT::identityFingerprint(idA.publicKey);
	m.fpB		   = PQFT::identityFingerprint(idB.publicKey);
	m.fileName	 = QStringLiteral("holiday photos.zip");
	m.mimeType	 = QStringLiteral("application/zip");
	m.fileSize	 = 3 * 256 * 1024 - 17;
	m.chunkSize	= 256 * 1024;
	m.chunkCount   = 3;
	m.merkleRoot   = PQFT::randomBytes(PQFT::HashSize);
	m.createdAtUnix = 1234567;
	m.wrappedFileKey = PQFT::randomBytes(PQFT::KeySize + PQFT::TagSize);
	QVERIFY(PQFT::signManifest(m, signA));

	const QByteArray encoded = PQFT::encodeManifest(m);
	QVERIFY(!encoded.isEmpty());

	PQFT::FTManifest parsed;
	QVERIFY(PQFT::parseAndVerifyManifest(parsed, encoded, m.fpA, 10ull * 1024 * 1024 * 1024, verifyA));
	QCOMPARE(parsed.fileName, m.fileName);
	QCOMPARE(parsed.chunkCount, quint64(3));
	QCOMPARE(parsed.transferDigest(), m.transferDigest()); // identical for recipients

	// fp mismatch against the pinned sender fails
	QVERIFY(!PQFT::parseAndVerifyManifest(parsed, encoded, PQFT::randomBytes(48),
										  10ull << 30, verifyA));
	// size ceiling enforced
	QVERIFY(!PQFT::parseAndVerifyManifest(parsed, encoded, m.fpA, 1024, verifyA));
}

void TestFileTransferSession::manifestTamperFails() {
	TestIdentity idA = generateIdentity();
	auto signA = [&idA](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
		PQFT::SigMLDSA65 s;
		return s.sign(sig, idA.secretKey, msg, ctx);
	};
	auto verifyA = [&idA](const QByteArray &msg, const QByteArray &sig, const QByteArray &ctx) {
		PQFT::SigMLDSA65 s;
		return s.verify(idA.publicKey, msg, sig, ctx);
	};

	PQFT::FTManifest m;
	m.transferId   = PQFT::randomBytes(PQFT::TransferIdSize);
	m.fpA		   = PQFT::identityFingerprint(idA.publicKey);
	m.fpB		   = PQFT::randomBytes(PQFT::HashSize);
	m.fileName	 = QStringLiteral("a.txt");
	m.mimeType	 = QStringLiteral("text/plain");
	m.fileSize	 = 16 * 1024;
	m.chunkSize	= 16 * 1024;
	m.chunkCount   = 1;
	m.merkleRoot   = PQFT::randomBytes(PQFT::HashSize);
	m.wrappedFileKey = PQFT::randomBytes(PQFT::KeySize + PQFT::TagSize);
	QVERIFY(PQFT::signManifest(m, signA));

	QByteArray encoded = PQFT::encodeManifest(m);
	// Flip a byte in the middle of the encoded manifest -> signature fails
	encoded[encoded.size() / 2] = encoded[encoded.size() / 2] ^ 0x01;

	PQFT::FTManifest parsed;
	QVERIFY(!PQFT::parseAndVerifyManifest(parsed, encoded, m.fpA, 10ull << 30, verifyA));

	// Path-traversal names rejected even with a valid signature
	PQFT::FTManifest evil = m;
	evil.fileName = QStringLiteral("../../etc/passwd");
	QVERIFY(PQFT::signManifest(evil, signA));
	QVERIFY(!PQFT::parseAndVerifyManifest(parsed, PQFT::encodeManifest(evil), m.fpA, 10ull << 30,
										  verifyA));
}

void TestFileTransferSession::chunkRoundTrip() {
	const QByteArray transferId = PQFT::randomBytes(PQFT::TransferIdSize);
	const QByteArray digest	 = PQFT::randomBytes(PQFT::HashSize);
	const QByteArray fileKey	 = PQFT::randomBytes(PQFT::KeySize);

	const QByteArray chunk = PQFT::randomBytes(256 * 1024);
	QByteArray ct;
	QVERIFY(PQFT::encryptChunk(ct, fileKey, transferId, digest, 0, 3, chunk));
	QCOMPARE(ct.size(), chunk.size() + PQFT::TagSize);

	QByteArray pt;
	QVERIFY(PQFT::decryptChunk(pt, fileKey, transferId, digest, 0, 3, ct));
	QCOMPARE(pt, chunk);

	// Wrong index (nonce/AAD binding) fails
	QVERIFY(!PQFT::decryptChunk(pt, fileKey, transferId, digest, 1, 3, ct));
	// Tampered ciphertext fails
	QByteArray bad = ct;
	bad[7] = bad[7] ^ 0x01;
	QVERIFY(!PQFT::decryptChunk(pt, fileKey, transferId, digest, 0, 3, bad));
	// Wrong transfer digest (cross-transfer splicing) fails
	QVERIFY(!PQFT::decryptChunk(pt, fileKey, transferId, PQFT::randomBytes(PQFT::HashSize), 0, 3, ct));
	// Wrong file key fails
	QVERIFY(!PQFT::decryptChunk(pt, PQFT::randomBytes(PQFT::KeySize), transferId, digest, 0, 3, ct));
}

void TestFileTransferSession::fileKeyDoubleWrap() {
	// Full §10 ladder in the no-password shape: file_key -> layer1 under the
	// session KEK; and in the password shape: -> layer2 under pw_wrap_key.
	const QByteArray transferId = PQFT::randomBytes(PQFT::TransferIdSize);
	const QByteArray fpA		  = PQFT::randomBytes(PQFT::HashSize);
	const QByteArray fpB		  = PQFT::randomBytes(PQFT::HashSize);
	const QByteArray fileKey	  = PQFT::randomBytes(PQFT::KeySize);
	const QByteArray sessionKek   = PQFT::randomBytes(PQFT::KeySize);
	const QByteArray wrapNonce1   = PQFT::randomBytes(PQFT::NonceSize);

	QByteArray layer1;
	QVERIFY(PQFT::wrapFileKeySessionLayer(layer1, fileKey, sessionKek, wrapNonce1, transferId,
										  false, fpA, fpB));
	QCOMPARE(layer1.size(), PQFT::KeySize + PQFT::TagSize);

	QByteArray recovered;
	QVERIFY(PQFT::unwrapFileKeySessionLayer(recovered, layer1, sessionKek, wrapNonce1, transferId,
											false, fpA, fpB));
	QCOMPARE(recovered, fileKey);

	// Session key alone can't touch the password-wrapped shape
	QByteArray pwKeyBytes = PQFT::randomBytes(PQFT::Argon2OutputSize);
	const QByteArray pwWrapKey = PQFT::passwordWrapKey(pwKeyBytes, transferId, fpA, fpB);
	const QByteArray nonce2	= PQFT::wrapNonce2(pwWrapKey);
	QCOMPARE(nonce2.size(), PQFT::NonceSize);

	// The double-wrap scenario: layer1 is wrapped with password_mode=true in
	// its AAD (the mode is bound into both wrap layers).
	QByteArray layer1Pw;
	QVERIFY(PQFT::wrapFileKeySessionLayer(layer1Pw, fileKey, sessionKek, wrapNonce1, transferId,
										  true, fpA, fpB));
	const QByteArray salt = PQFT::randomBytes(PQFT::Argon2SaltSize);
	QByteArray layer2;
	QVERIFY(PQFT::wrapLayer1WithPassword(layer2, layer1Pw, pwWrapKey, transferId, true, fpA, fpB,
										 PQFT::Argon2Params(), salt));
	QCOMPARE(layer2.size(), layer1.size() + PQFT::TagSize);

	// Unwrap with a WRONG password key fails, and looks identical to corruption
	QByteArray wrongKeyBytes = PQFT::randomBytes(PQFT::Argon2OutputSize);
	const QByteArray wrongWrapKey = PQFT::passwordWrapKey(wrongKeyBytes, transferId, fpA, fpB);
	QByteArray out1;
	QVERIFY(!PQFT::unwrapPasswordLayer(out1, layer2, wrongWrapKey, transferId, true, fpA, fpB,
									   PQFT::Argon2Params(), salt));
	QByteArray tampered = layer2;
	tampered[3] ^= 0x01;
	QByteArray out2;
	QVERIFY(!PQFT::unwrapPasswordLayer(out2, tampered, pwWrapKey, transferId, true, fpA, fpB,
									   PQFT::Argon2Params(), salt));

	// Right password key unwraps layer2 -> layer1 -> file_key
	QByteArray inner, finalKey;
	QVERIFY(PQFT::unwrapPasswordLayer(inner, layer2, pwWrapKey, transferId, true, fpA, fpB,
									  PQFT::Argon2Params(), salt));
	QVERIFY(PQFT::unwrapFileKeySessionLayer(finalKey, inner, sessionKek, wrapNonce1, transferId,
											true, fpA, fpB));
	QCOMPARE(finalKey, fileKey);
}
