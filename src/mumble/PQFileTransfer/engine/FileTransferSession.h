// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// One pairwise file-transfer session implementing the PQShield v2
// handshake (§6) and key schedule (§7) verbatim. Pure state machine: no
// sockets, no Qt event loop — the engine feeds it frame bytes and sends
// what it produces. See PROTOCOL.md §5.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERSESSION_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERSESSION_H_

#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <QByteArray>
#include <functional>

namespace PQFT {

/// Identity material the session needs from its owner: the long-term
/// ML-DSA-65 public key and a signing function (context-separated).
struct SessionIdentity {
	QByteArray publicKey;
	std::function< bool(QByteArray &signature, const QByteArray &message, const QByteArray &context) >
		sign;
};

class FileTransferSession {
public:
	enum class Role { Initiator, Responder };
	enum class State { Created, AwaitingM2, AwaitingM3, AwaitingM4, Established, Failed };

	/// `expectedPeerFingerprint` is the TOFU pin for the peer (SHA-384 of its
	/// identity key). The handshake aborts unless the presented identity key
	/// hashes to exactly this value — identity substitution is impossible by
	/// construction.
	FileTransferSession(Role role, SessionIdentity identity, QByteArray expectedPeerFingerprint);
	~FileTransferSession();

	FileTransferSession(const FileTransferSession &)            = delete;
	FileTransferSession &operator=(const FileTransferSession &) = delete;

	State state() const { return m_state; }
	bool established() const { return m_state == State::Established; }

	/// Fingerprint of the verified peer identity (valid after the peer's
	/// identity key was seen and matched the pin).
	const QByteArray &peerFingerprint() const { return m_expectedPeerFingerprint; }

	// --- Initiator (file sender, role A) ---------------------------------

	/// M1: version, suite, nonce_A, eph_x25519_pk_A, id_pk_A.
	QByteArray buildM1();

	/// Verify M2: suite match, pinned identity, sig_B over TH1.
	bool processM2(const QByteArray &frame);

	/// M3: ct_MLKEM, sig_A over TH_hs, MAC_A.
	QByteArray buildM3();

	/// Verify MAC_B → Established.
	bool processM4(const QByteArray &frame);

	// --- Responder (recipient, role B) ------------------------------------

	/// Verify M1 (suite, canonical form, pinned identity of A).
	bool processM1(const QByteArray &frame);

	/// M2: version, suite, nonce_B, eph_x25519_pk_B, eph_MLKEM_ek_B, id_pk_B,
	/// sig_B over TH1.
	QByteArray buildM2();

	/// Verify M3: sig_A over TH_hs, MAC_A; decapsulate; derive secrets.
	bool processM3(const QByteArray &frame);

	/// M4: MAC_B over TH_full.
	QByteArray buildM4();

	// --- Established: AEAD control channel (strict seq, per direction) ------

	/// Encrypt an outbound control body (canonical CBOR). Returns the frame
	/// to send. Empty QByteArray on failure.
	QByteArray sealControl(quint8 frameType, const QByteArray &canonicalBody);

	/// Decrypt an inbound control frame; enforces seq == expected.
	bool openControl(quint8 &frameType, QByteArray &canonicalBody, const QByteArray &frame);

	// --- Established: per-transfer derivations (§7 / §10) --------------------

	/// session_kek = Expand(PRK, "ft/filekey-session-wrap/" ‖ transfer_id ‖
	/// fp_A ‖ fp_B, 32) where fp_A is the initiator's and fp_B the
	/// responder's fingerprint.
	QByteArray deriveSessionKek(const QByteArray &transferId) const;

	/// wrap_nonce_1 = Expand(session_kek, "ft/wrap-nonce-1", 12).
	QByteArray deriveWrapNonce1(const QByteArray &transferId) const;

	/// Exports for tests only.
	QByteArray debugPrk() const { return m_prk.toByteArray(); }

private:
	struct HandshakeKeys {
		// own ephemerals
		SecureBytes x25519Secret;
		QByteArray x25519Public;
		// responder ML-KEM ephemeral
		SecureBytes mlkemSecret;
		QByteArray mlkemPublic;
	};

	void fail();
	void deriveEarlySecrets();     // PRK_early, finished keys (needs ss_x ‖ ss_k, TH_hs)
	void deriveTrafficSecrets();   // PRK, control keys/nonces (needs TH_full)
	QByteArray computeMacA() const;
	QByteArray computeMacB() const;

	Role m_role;
	SessionIdentity m_identity;
	QByteArray m_expectedPeerFingerprint;
	State m_state = State::Created;
	/// Each handshake message may be processed exactly once; duplicates are
	/// fatal (there is no legitimate retransmission on a reliable transport).
	bool m_m1Processed = false;
	bool m_m2Processed = false;
	bool m_m3Processed = false;
	bool m_m4Processed = false;

	// transcript members
	QByteArray m_m1Encoding;
	QByteArray m_m2Encoding;         // full M2 (with sig_B)
	QByteArray m_m2WithoutSig;       // M2 minus the sig_B field
	QByteArray m_mlkemCiphertext;    // ct_MLKEM
	QByteArray m_sigA;
	QByteArray m_macA;
	QByteArray m_thHs;               // TH_hs
	QByteArray m_thFull;             // TH_full

	// secrets
	HandshakeKeys m_ephemeral;
	QByteArray m_ephemeralPeerX25519; // peer's ephemeral X25519 public key
	QByteArray m_peerMlKemEk;         // initiator side: responder's ML-KEM ek
	QByteArray m_nonceA, m_nonceB;
	QByteArray m_peerIdentityKey;    // verified against the pin
	SecureBytes m_ssX25519;
	SecureBytes m_ssMlKem;
	SecureBytes m_prkEarly;
	SecureBytes m_finishedKeyA2B, m_finishedKeyB2A;
	SecureBytes m_prk;
	QByteArray m_controlKeyOut, m_controlKeyIn;
	QByteArray m_noncePrefixOut, m_noncePrefixIn;
	quint64 m_seqOut  = 0;
	quint64 m_seqIn   = 0;
	bool m_seqInStarted = false;
};

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERSESSION_H_
