// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferSession.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/KemMLKEM768.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FTMessages.h"
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QCborMap>
#include <QPair>
#include <QVector>

namespace PQFT {
namespace {

using MapEntries = QVector< QPair< QCborValue, QCborValue > >;

// CBOR field keys in the fixed order of the spec (§6)
constexpr int KVersion   = 1;
constexpr int KSuite	   = 2;
constexpr int KNonce	   = 3;
constexpr int KEphX25519  = 4;
constexpr int KMlKemEk	= 5;
constexpr int KIdentityPk = 6;
constexpr int KSignature  = 7;
// M3 carries its own fields
constexpr int K3Ciphertext = 1;
constexpr int K3SigA		 = 2;
constexpr int K3MacA		 = 3;
constexpr int K4MacB		 = 1;

MapEntries versionAndSuite() {
	MapEntries entries;
	entries.append({ QCborValue(KVersion), QCborValue(Version) });
	entries.append({ QCborValue(KSuite), QCborValue(QString::fromLatin1(SuiteId)) });
	return entries;
}

QByteArray mapBytes(const QCborMap &map, int key) {
	return map.value(QCborValue(key)).toByteArray();
}

} // namespace

FileTransferSession::FileTransferSession(Role role, SessionIdentity identity,
										 QByteArray expectedPeerFingerprint)
	: m_role(role), m_identity(std::move(identity)),
	  m_expectedPeerFingerprint(std::move(expectedPeerFingerprint)) { }

FileTransferSession::~FileTransferSession() {
	// Secret members are SecureBytes (self-zeroizing); public material needs
	// no scrubbing.
}

void FileTransferSession::fail() {
	m_state			 = State::Failed;
	m_ssX25519.clear();
	m_ssMlKem.clear();
	m_prkEarly.clear();
	m_prk.clear();
	m_finishedKeyA2B.clear();
	m_finishedKeyB2A.clear();
	m_controlKeyIn.clear();
	m_controlKeyOut.clear();
	m_noncePrefixIn.clear();
	m_noncePrefixOut.clear();
}

// --- Initiator ------------------------------------------------------------------

QByteArray FileTransferSession::buildM1() {
	if (m_state != State::Created || m_role != Role::Initiator)
		return QByteArray();

	if (!x25519GenerateKeyPair(m_ephemeral.x25519Public, m_ephemeral.x25519Secret))
		return fail(), QByteArray();

	m_nonceA = randomBytes(HandshakeNonceSize);

	MapEntries m1 = versionAndSuite();
	m1.append({ QCborValue(KNonce), QCborValue(m_nonceA) });
	m1.append({ QCborValue(KEphX25519), QCborValue(m_ephemeral.x25519Public) });
	m1.append({ QCborValue(KIdentityPk), QCborValue(m_identity.publicKey) });

	m_m1Encoding = encodeCanonicalMap(m1);
	if (m_m1Encoding.isEmpty())
		return fail(), QByteArray();

	m_state = State::AwaitingM2;
	return FTFrame::encodeHandshake(FTFrame::TypeM1, m_m1Encoding);
}

bool FileTransferSession::processM2(const QByteArray &frame) {
	if (m_state != State::AwaitingM2 || m_role != Role::Initiator || m_m2Processed)
		return fail(), false;
	m_m2Processed = true;

	quint8 type	  = 0;
	QCborValue value;
	if (!FTFrame::decodeHeader(frame, type) || type != FTFrame::TypeM2
		|| !decodeCanonical(frame.mid(1), value)) {
		return fail(), false;
	}
	const QCborMap m2 = value.toMap();
	if (m2.size() != 7)
		return fail(), false;

	if (m2.value(QCborValue(KVersion)).toInteger() != Version
		|| m2.value(QCborValue(KSuite)).toString() != QString::fromLatin1(SuiteId)) {
		return fail(), false;
	}

	const QByteArray nonceB = mapBytes(m2, KNonce);
	const QByteArray ephB   = mapBytes(m2, KEphX25519);
	const QByteArray ekB	= mapBytes(m2, KMlKemEk);
	const QByteArray peerPk = mapBytes(m2, KIdentityPk);
	const QByteArray sigB   = mapBytes(m2, KSignature);

	if (nonceB.size() != HandshakeNonceSize || ephB.size() != X25519KeySize
		|| ekB.size() != KemMLKEM768::PublicKeySize || peerPk.size() != SigMLDSA65::PublicKeySize
		|| sigB.isEmpty()) {
		return fail(), false;
	}

	// Identity pin: the presented key must hash to exactly the pinned
	// fingerprint — no silent updates, ever (§5).
	if (identityFingerprint(peerPk) != m_expectedPeerFingerprint) {
		return fail(), false;
	}
	m_peerIdentityKey   = peerPk;
	m_nonceB			= nonceB;
	m_ephemeralPeerX25519 = ephB;
	m_peerMlKemEk		= ekB;
	m_m2Encoding		= frame.mid(1);

	// TH1 = H(M1 ‖ M2-without-sig)
	MapEntries withoutSig = versionAndSuite();
	withoutSig.append({ QCborValue(KNonce), QCborValue(nonceB) });
	withoutSig.append({ QCborValue(KEphX25519), QCborValue(ephB) });
	withoutSig.append({ QCborValue(KMlKemEk), QCborValue(ekB) });
	withoutSig.append({ QCborValue(KIdentityPk), QCborValue(peerPk) });
	m_m2WithoutSig = encodeCanonicalMap(withoutSig);
	if (m_m2WithoutSig.isEmpty())
		return fail(), false;

	const QByteArray th1 = sha384({ m_m1Encoding, m_m2WithoutSig });

	SigMLDSA65 sig;
	if (!sig.verify(peerPk, th1, sigB, QByteArray(LabelHandshakeCtx))) {
		return fail(), false;
	}
	return true;
}

QByteArray FileTransferSession::buildM3() {
	if (m_state != State::AwaitingM2 || m_role != Role::Initiator)
		return QByteArray();

	KemMLKEM768 kem;
	if (!kem.encaps(m_mlkemCiphertext, m_ssMlKem, m_peerMlKemEk)) {
		return fail(), QByteArray();
	}

	const QByteArray ssX =
		x25519SharedSecret(m_ephemeral.x25519Secret.toByteArray(), m_ephemeralPeerX25519);
	if (ssX.isEmpty() || isAllZero(ssX)) {
		return fail(), QByteArray();
	}
	m_ssX25519 = SecureBytes::fromByteArray(ssX);

	m_thHs = sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext });
	deriveEarlySecrets();

	if (!m_identity.sign(m_sigA, m_thHs, QByteArray(LabelHandshakeCtx))) {
		return fail(), QByteArray();
	}

	m_macA   = computeMacA();
	m_thFull = sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext, m_sigA, m_macA });

	MapEntries m3;
	m3.append({ QCborValue(K3Ciphertext), QCborValue(m_mlkemCiphertext) });
	m3.append({ QCborValue(K3SigA), QCborValue(m_sigA) });
	m3.append({ QCborValue(K3MacA), QCborValue(m_macA) });
	const QByteArray encoded = encodeCanonicalMap(m3);
	if (encoded.isEmpty())
		return fail(), QByteArray();

	m_state = State::AwaitingM4;
	return FTFrame::encodeHandshake(FTFrame::TypeM3, encoded);
}

bool FileTransferSession::processM4(const QByteArray &frame) {
	if (m_state != State::AwaitingM4 || m_role != Role::Initiator || m_m4Processed)
		return fail(), false;
	m_m4Processed = true;

	quint8 type	  = 0;
	QCborValue value;
	if (!FTFrame::decodeHeader(frame, type) || type != FTFrame::TypeM4
		|| !decodeCanonical(frame.mid(1), value)) {
		return fail(), false;
	}
	const QCborMap m4 = value.toMap();
	if (m4.size() != 1)
		return fail(), false;

	const QByteArray macB = mapBytes(m4, K4MacB);
	if (macB.size() != FinishedKeySize || !constantTimeEquals(macB, computeMacB())) {
		return fail(), false;
	}

	deriveTrafficSecrets();
	m_state = State::Established;
	return true;
}

// --- Responder --------------------------------------------------------------------

bool FileTransferSession::processM1(const QByteArray &frame) {
	if (m_state != State::Created || m_role != Role::Responder || m_m1Processed)
		return fail(), false;
	m_m1Processed = true;

	quint8 type	  = 0;
	QCborValue value;
	if (!FTFrame::decodeHeader(frame, type) || type != FTFrame::TypeM1
		|| !decodeCanonical(frame.mid(1), value)) {
		return fail(), false;
	}
	const QCborMap m1 = value.toMap();
	if (m1.size() != 5)
		return fail(), false;

	if (m1.value(QCborValue(KVersion)).toInteger() != Version
		|| m1.value(QCborValue(KSuite)).toString() != QString::fromLatin1(SuiteId)) {
		return fail(), false;
	}

	const QByteArray nonceA = mapBytes(m1, KNonce);
	const QByteArray ephA   = mapBytes(m1, KEphX25519);
	const QByteArray peerPk = mapBytes(m1, KIdentityPk);

	if (nonceA.size() != HandshakeNonceSize || ephA.size() != X25519KeySize
		|| peerPk.size() != SigMLDSA65::PublicKeySize) {
		return fail(), false;
	}

	if (identityFingerprint(peerPk) != m_expectedPeerFingerprint) {
		return fail(), false;
	}
	m_peerIdentityKey	  = peerPk;
	m_nonceA			  = nonceA;
	m_ephemeralPeerX25519 = ephA;
	m_m1Encoding		  = frame.mid(1);
	return true;
}

QByteArray FileTransferSession::buildM2() {
	if (m_state != State::Created || m_role != Role::Responder)
		return QByteArray();

	KemMLKEM768 kem;
	if (!x25519GenerateKeyPair(m_ephemeral.x25519Public, m_ephemeral.x25519Secret)
		|| !kem.keypair(m_ephemeral.mlkemPublic, m_ephemeral.mlkemSecret)) {
		return fail(), QByteArray();
	}
	m_nonceB = randomBytes(HandshakeNonceSize);

	MapEntries withoutSig = versionAndSuite();
	withoutSig.append({ QCborValue(KNonce), QCborValue(m_nonceB) });
	withoutSig.append({ QCborValue(KEphX25519), QCborValue(m_ephemeral.x25519Public) });
	withoutSig.append({ QCborValue(KMlKemEk), QCborValue(m_ephemeral.mlkemPublic) });
	withoutSig.append({ QCborValue(KIdentityPk), QCborValue(m_identity.publicKey) });
	m_m2WithoutSig = encodeCanonicalMap(withoutSig);
	if (m_m2WithoutSig.isEmpty())
		return fail(), QByteArray();

	const QByteArray th1 = sha384({ m_m1Encoding, m_m2WithoutSig });

	QByteArray sigB;
	if (!m_identity.sign(sigB, th1, QByteArray(LabelHandshakeCtx))) {
		return fail(), QByteArray();
	}

	MapEntries m2 = withoutSig;
	m2.append({ QCborValue(KSignature), QCborValue(sigB) });
	m_m2Encoding = encodeCanonicalMap(m2);
	if (m_m2Encoding.isEmpty())
		return fail(), QByteArray();

	m_state = State::AwaitingM3;
	return FTFrame::encodeHandshake(FTFrame::TypeM2, m_m2Encoding);
}

bool FileTransferSession::processM3(const QByteArray &frame) {
	if (m_state != State::AwaitingM3 || m_role != Role::Responder || m_m3Processed)
		return fail(), false;
	m_m3Processed = true;

	quint8 type	  = 0;
	QCborValue value;
	if (!FTFrame::decodeHeader(frame, type) || type != FTFrame::TypeM3
		|| !decodeCanonical(frame.mid(1), value)) {
		return fail(), false;
	}
	const QCborMap m3 = value.toMap();
	if (m3.size() != 3)
		return fail(), false;

	m_mlkemCiphertext = mapBytes(m3, K3Ciphertext);
	m_sigA			  = mapBytes(m3, K3SigA);
	const QByteArray macA = mapBytes(m3, K3MacA);

	if (m_mlkemCiphertext.size() != KemMLKEM768::CiphertextSize || m_sigA.isEmpty()
		|| macA.size() != FinishedKeySize) {
		return fail(), false;
	}

	m_thHs = sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext });

	// Verify A's signature before deriving anything expensive (§10 DoS rule)
	SigMLDSA65 sig;
	if (!sig.verify(m_peerIdentityKey, m_thHs, m_sigA, QByteArray(LabelHandshakeCtx))) {
		return fail(), false;
	}

	KemMLKEM768 kem;
	if (!kem.decaps(m_ssMlKem, m_mlkemCiphertext, m_ephemeral.mlkemSecret)) {
		return fail(), false;
	}
	const QByteArray ssX =
		x25519SharedSecret(m_ephemeral.x25519Secret.toByteArray(), m_ephemeralPeerX25519);
	if (ssX.isEmpty() || isAllZero(ssX)) {
		return fail(), false;
	}
	m_ssX25519 = SecureBytes::fromByteArray(ssX);

	deriveEarlySecrets();

	m_macA   = macA;
	m_thFull = sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext, m_sigA, m_macA });
	if (!constantTimeEquals(macA, computeMacA())) {
		return fail(), false;
	}
	return true;
}

QByteArray FileTransferSession::buildM4() {
	if (m_state != State::AwaitingM3 || m_role != Role::Responder)
		return QByteArray();

	MapEntries m4;
	m4.append({ QCborValue(K4MacB), QCborValue(computeMacB()) });
	const QByteArray encoded = encodeCanonicalMap(m4);
	if (encoded.isEmpty())
		return fail(), QByteArray();

	deriveTrafficSecrets();
	m_state = State::Established;
	return FTFrame::encodeHandshake(FTFrame::TypeM4, encoded);
}

// --- Established ----------------------------------------------------------------------

QByteArray FileTransferSession::sealControl(quint8 frameType, const QByteArray &canonicalBody) {
	if (m_state != State::Established || canonicalBody.isEmpty()) {
		return QByteArray();
	}

	const QByteArray nonce = FTFrame::controlNonce(m_noncePrefixOut, m_seqOut);

	// AAD = header || seq (built here so the ciphertext can be appended after)
	QByteArray aad;
	aad.reserve(9);
	aad.append(static_cast< char >((Version << 4) | (frameType & 0x0f)));
	aad.append(uint64be(m_seqOut));

	QByteArray ciphertext;
	if (!aesGcmEncrypt(ciphertext, m_controlKeyOut, nonce, canonicalBody, aad)) {
		return fail(), QByteArray();
	}

	QByteArray frame;
	frame.reserve(9 + ciphertext.size());
	frame.append(aad);
	frame.append(ciphertext);
	++m_seqOut;
	return frame;
}

bool FileTransferSession::openControl(quint8 &frameType, QByteArray &canonicalBody,
									  const QByteArray &frame) {
	if (m_state != State::Established)
		return false;

	quint8 type	 = 0;
	quint64 seq	 = 0;
	QByteArray ciphertext;
	if (!FTFrame::decodeHeader(frame, type) || type < FTFrame::TypeManifest
		|| !FTFrame::decodeControl(frame, seq, ciphertext)) {
		return false;
	}

	// Strict sequencing: first record must be 0, then exactly previous+1 (§8)
	if (!m_seqInStarted) {
		if (seq != 0)
			return fail(), false;
		m_seqInStarted = true;
	} else if (seq != m_seqIn + 1) {
		return fail(), false;
	}

	QByteArray plaintext;
	if (!aesGcmDecrypt(plaintext, m_controlKeyIn, FTFrame::controlNonce(m_noncePrefixIn, seq),
					   ciphertext, FTFrame::controlAAD(frame))) {
		return fail(), false;
	}
	QCborValue parsed;
	if (!decodeCanonical(plaintext, parsed)) {
		return fail(), false;
	}

	m_seqIn		= seq;
	frameType	= type;
	canonicalBody = plaintext;
	return true;
}

QByteArray FileTransferSession::deriveSessionKek(const QByteArray &transferId) const {
	if (m_state != State::Established || transferId.size() != TransferIdSize)
		return QByteArray();

	// fp_A is the initiator's fingerprint, fp_B the responder's — regardless
	// of which side we are.
	QByteArray fpA, fpB;
	if (m_role == Role::Initiator) {
		fpA = identityFingerprint(m_identity.publicKey);
		fpB = m_expectedPeerFingerprint;
	} else {
		fpA = m_expectedPeerFingerprint;
		fpB = identityFingerprint(m_identity.publicKey);
	}
	const QByteArray info = QByteArray(LabelSessionWrap) + transferId + fpA + fpB;
	return hkdfExpand(m_prk.toByteArray(), info, KeySize);
}

QByteArray FileTransferSession::deriveWrapNonce1(const QByteArray &transferId) const {
	const QByteArray sessionKek = deriveSessionKek(transferId);
	if (sessionKek.isEmpty())
		return QByteArray();
	return hkdfExpand(sessionKek, QByteArray(LabelWrapNonce1), NonceSize);
}

// --- Internal derivations ----------------------------------------------------------------

void FileTransferSession::deriveEarlySecrets() {
	QByteArray ikm;
	ikm.reserve(m_ssX25519.size() + m_ssMlKem.size());
	ikm.append(m_ssX25519.toByteArray());
	ikm.append(m_ssMlKem.toByteArray());

	m_prkEarly = SecureBytes::fromByteArray(hkdfExtract(m_thHs, ikm));
	zeroize(ikm);

	m_finishedKeyA2B = SecureBytes::fromByteArray(
		hkdfExpand(m_prkEarly.toByteArray(), QByteArray(LabelFinishedA2B) + m_thHs, FinishedKeySize));
	m_finishedKeyB2A = SecureBytes::fromByteArray(
		hkdfExpand(m_prkEarly.toByteArray(), QByteArray(LabelFinishedB2A) + m_thHs, FinishedKeySize));
}

void FileTransferSession::deriveTrafficSecrets() {
	m_prk = SecureBytes::fromByteArray(hkdfExtract(m_thFull, m_prkEarly.toByteArray()));

	const QByteArray a2b = hkdfExpand(m_prk.toByteArray(), QByteArray(LabelTrafficA2B) + m_thFull,
									  TrafficSecretSize);
	const QByteArray b2a = hkdfExpand(m_prk.toByteArray(), QByteArray(LabelTrafficB2A) + m_thFull,
									  TrafficSecretSize);

	const QByteArray keyA2b   = hkdfExpand(a2b, QByteArray(LabelControlKey), KeySize);
	const QByteArray nonceA2b = hkdfExpand(a2b, QByteArray(LabelControlNonce), 4);
	const QByteArray keyB2a   = hkdfExpand(b2a, QByteArray(LabelControlKey), KeySize);
	const QByteArray nonceB2a = hkdfExpand(b2a, QByteArray(LabelControlNonce), 4);

	// Outbound = toward the peer: the initiator speaks a2b, the responder b2a
	m_controlKeyOut = m_role == Role::Initiator ? keyA2b : keyB2a;
	m_noncePrefixOut = m_role == Role::Initiator ? nonceA2b : nonceB2a;
	m_controlKeyIn  = m_role == Role::Initiator ? keyB2a : keyA2b;
	m_noncePrefixIn = m_role == Role::Initiator ? nonceB2a : nonceA2b;
}

QByteArray FileTransferSession::computeMacA() const {
	// TH_fin = H(M1 ‖ M2 ‖ ct ‖ sig_A)
	const QByteArray thFin = sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext, m_sigA });
	return hmacSha384(m_finishedKeyA2B.toByteArray(), { thFin });
}

QByteArray FileTransferSession::computeMacB() const {
	// TH_full = H(M1 ‖ M2 ‖ ct ‖ sig_A ‖ MAC_A); MAC_B = HMAC(fk_b2a, TH_full)
	const QByteArray thFull =
		sha384({ m_m1Encoding, m_m2Encoding, m_mlkemCiphertext, m_sigA, m_macA });
	return hmacSha384(m_finishedKeyB2A.toByteArray(), { thFull });
}

} // namespace PQFT
