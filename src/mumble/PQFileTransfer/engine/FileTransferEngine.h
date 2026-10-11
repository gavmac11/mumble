// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// FileTransferEngine: the worker-thread half of the file-transfer feature.
// Owns pairwise sessions, send/receive jobs, pacing and timeouts. Talks to
// the world exclusively through transport callbacks (set by the manager)
// and queued signals — never to sockets or widgets. See PROTOCOL.md.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERENGINE_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERENGINE_H_

#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/engine/FTManifest.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

class QFile;
class QFileDevice;
class TestFileTransferEngine;

namespace PQFT {

class FileTransferSession;

/// Everything the UI needs to draw one transfer (queued from the engine).
struct FTTransferInfo {
	enum class State {
		Handshaking,
		VerifyingIdentity,   // first contact, safety-number dialog pending
		WaitingPassword,	 // password required to decrypt
		Transferring,
		Verifying,		   // all bytes in, Merkle check running/ran
		Ready,			   // verified, waiting for the user to save
		Saved,
		Failed,
		Aborted,
	};

	QByteArray transferId;
	unsigned int peerSession = 0;   // remote peer (sender when receiving)
	bool incoming			  = false;
	QString fileName;
	QString mimeType;
	quint64 fileSize		  = 0;
	quint64 bytesDone		  = 0;
	bool passwordMode		  = false;
	State state				  = State::Handshaking;
	QString error;				  // generic where it matters (§13)
};

class FileTransferEngine : public QObject {
	Q_OBJECT

public:
	/// Transports — the manager wires these to the ServerHandler (queued).
	using TransportControl = std::function< void(unsigned int targetSession, const QByteArray &payload) >;
	using TransportChunk =
		std::function< void(const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) >;
	/// TOFU pin lookup: returns the pinned fingerprint for a peer session or
	/// an empty QByteArray on first contact (mutex-guarded inside the manager).
	using PinLookup = std::function< QByteArray(unsigned int peerSession) >;
	/// Peer identity for signing (thread-safe part of FileTransferIdentity).
	using IdentitySign =
		std::function< bool(QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) >;

	struct Config {
		// Maximum plaintext chunk size; low rates use smaller protocol-valid chunks.
		quint32 chunkSize  = 256 * 1024;
		// Shared ciphertext-byte budget across all sends; 0 = unlimited.
		quint32 sendRateBytesPerSecond = 4 * 1024 * 1024;
		quint64 maxReceiveSize = 10ull * 1024 * 1024 * 1024;
		int handshakeTimeoutMSecs	  = 10000;
		int receiveIdleTimeoutMSecs   = 60000;
	};

	explicit FileTransferEngine(QObject *parent = nullptr);
	~FileTransferEngine() override;

	void setTransport(TransportControl control, TransportChunk chunk);
	void setPinLookup(PinLookup lookup);
	void setIdentity(QByteArray identityPublicKey, IdentitySign sign);
	/// Call in the engine's owning thread; the manager queues configuration updates.
	void setConfig(const Config &config);

	// ---- Inbound (queued from the manager; ordering preserved) ----
	void onControlMessage(unsigned int actorSession, const QByteArray &payload);
	void onDataMessage(unsigned int actorSession, const QByteArray &transferId, quint64 chunkIndex,
					   quint64 chunkCountHint, const QByteArray &data);

	// ---- Outbound requests (queued from the manager) ----
	quint64 startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
					  QByteArray password, const QSet< unsigned int > &recipients);
	void abortTransfer(const QByteArray &transferId);
	/// Existing targets are replaced only after explicit user confirmation.
	/// Otherwise creation is exclusive; failed writes leave the receive retryable.
	void saveTransferAs(const QByteArray &transferId, const QString &targetPath, bool replaceConfirmed = false);
	void providePassword(const QByteArray &transferId, QByteArray password);
	/// Resume a first-contact handshake the user verified (verified=true) or
	/// drop it (verified=false).
	void resolveFirstContact(unsigned int peerSession, bool verified);
	/// Abort every in-flight transfer (disconnect / server change).
	void abortAll();

signals:
	void transferUpdated(const PQFT::FTTransferInfo &info);
	/// pinOnObservation: the handshake already authenticated the peer's
	/// identity key (sender side, after M4) — TOFU pinning on first
	/// observation is the documented deviation. When false (receiver side,
	/// plain M1) the fingerprint is an unauthenticated claim and MUST only be
	/// pinned once the user completes the safety-number verification.
	void firstContact(unsigned int peerSession, const QByteArray &peerFingerprint,
					  const QString &safetyNumber, const QByteArray &pendingTransferId,
					  bool pinOnObservation);
	void peerBlocked(unsigned int peerSession);
	void passwordRequired(const QByteArray &transferId);

private:
	friend class ::TestFileTransferEngine;
	std::function< bool(QFileDevice &) > m_syncSaveFile;
	std::function< bool(const QString &) > m_syncSavePublication;
	// --- sending ---
	struct SendPeer {
		unsigned int session = 0;
		/// The pinned fingerprint at send time; empty = first use (pin on
		/// first observation after the signature verifies).
		QByteArray pinnedFingerprint;
		/// Raw pointer: the session is owned by the SendJob (freed when the
		/// job ends) — a plain member keeps SendPeer copyable for QList.
		FileTransferSession *session_ = nullptr;
		bool established	  = false;
		bool failed		   = false;
	};
	struct SendJob {
		QByteArray transferId;
		QString sourcePath;
		QString fileName;
		QString mimeType;
		bool passwordMode  = false;
		QByteArray password;   // zeroized once the wrap is done
		QList< SendPeer > peers;
		QByteArray fileKey;
		QVector< QByteArray > chunkDigests;
		::QFile *file = nullptr;           // kept open until the send finishes
		QByteArray merkleRoot;
		quint64 fileSize		= 0;
		quint64 bytesDone		= 0;
		quint64 chunkCount		= 0;
		quint64 nextChunkIndex = 0;
		quint32 effectiveChunkSize = 0;
		quint64 lastProgressBytes  = 0;
		QByteArray transferDigest;
		bool manifestSent		= false;
		std::unique_ptr< QTimer > handshakeTimer;
		FTTransferInfo::State lastState = FTTransferInfo::State::Handshaking;
	};

	// --- receiving ---
	struct ReceiveJob {
		QByteArray transferId;
		unsigned int peerSession = 0;
		FTManifest manifest;
		bool haveManifest		= false;
		QString fileName;
		QString mimeType;
		quint64 fileSize		= 0;
		quint64 bytesDone		= 0;
		quint64 lastProgressBytes = 0;
		bool passwordMode		= false;
		QByteArray fileKey;   // recovered (possibly after password)
		bool waitingPassword   = false;
		// Own a random, private receive directory; destruction removes unsaved data.
		std::unique_ptr< QTemporaryDir > tempDirectory;
		QString tempDir;
		QString tempFile;
		/// Ciphertext spool while the password is pending: the sender keeps
		/// streaming (no readiness acknowledgement exists), so every chunk
		/// must survive on disk until the key can be unwrapped.
		QString spoolFile;
		::QFile *spool = nullptr;  // open for appending while waitingPassword
		QByteArray receivedBits;   // bit i set = chunk i verified
		quint64 receivedCount  = 0;
		QByteArray spooledBits;   // bit i set = chunk i spooled while a password was pending
		quint64 spoolBytes	= 0;   // bytes written to the spool so far
		QVector< QByteArray > leafHashes;   // chunk digests (merkleRoot builds the leaves)
		struct EarlyChunk {
			QByteArray transferId;
			quint64 index;
			QByteArray ciphertext;
		};
		QVector< EarlyChunk > earlyChunks;
		std::unique_ptr< QTimer > idleTimer;
		FTTransferInfo::State lastState = FTTransferInfo::State::Handshaking;
		std::unique_ptr< FileTransferSession > session_;   // responder side
	};

	// --- pending incoming handshakes (before the transfer exists) ---
	struct PendingHandshake {
		unsigned int peerSession = 0;
		QByteArray peerFingerprint;
		QByteArray m1Frame;
		QTimer *timeout = nullptr;   // parented to the engine
	};
	static void stopPendingHandshakeTimer(PendingHandshake &pending);

	void processControlForSend(SendJob &job, SendPeer &peer, const QByteArray &payload);
	/// Handles an already-authenticated control record (routed by
	/// onControlMessage, which probed the sessions non-destructively).
	void processControlForReceive(std::shared_ptr< ReceiveJob > jobPtr, quint8 type,
								  const QByteArray &body);
	void startResponder(unsigned int actorSession, const QByteArray &m1Frame,
						const QByteArray &peerFingerprint);
	void handleIncomingM1(unsigned int actorSession, const QByteArray &payload);
	void maybeStartHandshakePhase2(SendJob &job);
	void buildAndSendManifests(SendJob &job);
	void paceSends();
	void refillSendCredit();
	void sendNextChunk(SendJob &job);
	void finishSend(SendJob &job, bool success, const QString &error);
	void updateSendState(SendJob &job, FTTransferInfo::State state, const QString &error = QString());
	void updateReceiveState(ReceiveJob &job, FTTransferInfo::State state, const QString &error = QString());
	/// Takes the owning pointer: failure paths remove the job from the map,
	/// which may drop the last reference — the parameter keeps it alive.
	void feedReceiveChunk(std::shared_ptr< ReceiveJob > jobPtr, quint64 index,
						  const QByteArray &ciphertext);
	void drainEarlyChunks(std::shared_ptr< ReceiveJob > jobPtr);
	void drainSpooledChunks(std::shared_ptr< ReceiveJob > jobPtr);
	bool openSpool(ReceiveJob &job);
	bool tryCompleteReceive(std::shared_ptr< ReceiveJob > jobPtr);
	void cleanupSend(SendJob &job, bool keepCard);
	/// Removes the job from the map first (dropping the map's owning
	/// reference) and keeps it alive through the pointer for the rest of the
	/// teardown — reading job fields after the erase used to be a
	/// use-after-free.
	void cleanupReceive(std::shared_ptr< ReceiveJob > jobPtr, bool keepReady = true);
	void emitInfo(const FTTransferInfo &info);
	void emitSaveDone(std::shared_ptr< ReceiveJob > jobPtr);
	void emitSaveFailed(ReceiveJob &job, const QString &error = QString());
	std::shared_ptr< ReceiveJob > findReceiveByPeer(unsigned int peerSession,
													const QByteArray &transferId);
	/// Key for receive jobs whose manifest (and thus transferId) has not
	/// arrived yet.
	static QByteArray preManifestKey(unsigned int peerSession);

	Config m_config;
	QByteArray m_identityPk;
	IdentitySign m_sign;
	TransportControl m_transportControl;
	TransportChunk m_transportChunk;
	PinLookup m_pinLookup;
	std::unique_ptr< QTimer > m_sendPaceTimer;
	QElapsedTimer m_sendPaceClock;
	qint64 m_sendCreditMilliBytes = 0;
	qint64 m_sendCreditLastNSecs  = 0;
	qint64 m_sendCreditRemainder  = 0;
	QList< QByteArray > m_sendPaceOrder;

	QHash< QByteArray, std::shared_ptr< SendJob > > m_sendJobs;
	QHash< QByteArray, std::shared_ptr< ReceiveJob > > m_receiveJobs;
	QHash< unsigned int, PendingHandshake > m_pendingHandshakes;   // responder side, pre-verification
	quint64 m_nextJobNonce = 0;
};

} // namespace PQFT

Q_DECLARE_METATYPE(PQFT::FTTransferInfo)

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERENGINE_H_
