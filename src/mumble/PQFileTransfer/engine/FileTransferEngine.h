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
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>

class QFile;

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
		quint32 chunkSize  = 256 * 1024;
		quint32 sendRateBytesPerSecond = 4 * 1024 * 1024;   // 0 = unlimited
		quint64 maxReceiveSize = 10ull * 1024 * 1024 * 1024;
		int handshakeTimeoutMSecs	  = 10000;
		int receiveIdleTimeoutMSecs   = 60000;
	};

	explicit FileTransferEngine(QObject *parent = nullptr);
	~FileTransferEngine() override;

	void setTransport(TransportControl control, TransportChunk chunk);
	void setPinLookup(PinLookup lookup);
	void setIdentity(QByteArray identityPublicKey, IdentitySign sign);
	void setConfig(const Config &config);

	// ---- Inbound (queued from the manager; ordering preserved) ----
	void onControlMessage(unsigned int actorSession, const QByteArray &payload);
	void onDataMessage(unsigned int actorSession, const QByteArray &transferId, quint64 chunkIndex,
					   quint64 chunkCountHint, const QByteArray &data);

	// ---- Outbound requests (queued from the manager) ----
	quint64 startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
					  QByteArray password, const QSet< unsigned int > &recipients);
	void abortTransfer(const QByteArray &transferId);
	/// Target must be on the same directory the user chose; engine verifies.
	void saveTransferAs(const QByteArray &transferId, const QString &targetPath);
	void providePassword(const QByteArray &transferId, QByteArray password);
	/// Resume a first-contact handshake the user verified (verified=true) or
	/// drop it (verified=false).
	void resolveFirstContact(unsigned int peerSession, bool verified);
	/// Abort every in-flight transfer (disconnect / server change).
	void abortAll();

signals:
	void transferUpdated(const PQFT::FTTransferInfo &info);
	void firstContact(unsigned int peerSession, const QByteArray &peerFingerprint,
					  const QString &safetyNumber, const QByteArray &pendingTransferId);
	void peerBlocked(unsigned int peerSession);
	void passwordRequired(const QByteArray &transferId);

private:
	// --- sending ---
	struct SendPeer {
		unsigned int session = 0;
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
		std::unique_ptr< QTimer > paceTimer;
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
		QString tempDir;
		QString tempFile;
		QByteArray receivedBits;   // bit i set = chunk i verified
		quint64 receivedCount  = 0;
		QVector< QByteArray > leafHashes;   // chunk digests (merkleRoot builds the leaves)
		QVector< QByteArray > earlyChunks;   // "u64be index" || ciphertext blobs
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

	void processControlForSend(SendJob &job, SendPeer &peer, const QByteArray &payload);
	void processControlForReceive(const std::shared_ptr< ReceiveJob > &jobPtr, const QByteArray &payload);
	void startResponder(unsigned int actorSession, const QByteArray &m1Frame,
						const QByteArray &peerFingerprint);
	void handleIncomingM1(unsigned int actorSession, const QByteArray &payload);
	void maybeStartHandshakePhase2(SendJob &job);
	void buildAndSendManifests(SendJob &job);
	void sendNextChunks(SendJob &job, qint64 budgetBytes);
	void finishSend(SendJob &job, bool success, const QString &error);
	void updateSendState(SendJob &job, FTTransferInfo::State state, const QString &error = QString());
	void updateReceiveState(ReceiveJob &job, FTTransferInfo::State state, const QString &error = QString());
	void feedReceiveChunk(ReceiveJob &job, quint64 index, const QByteArray &ciphertext);
	void drainEarlyChunks(ReceiveJob &job);
	bool tryCompleteReceive(ReceiveJob &job);
	void cleanupSend(SendJob &job, bool keepCard);
	void cleanupReceive(ReceiveJob &job);
	void emitInfo(const FTTransferInfo &info);
	void emitSaveDone(ReceiveJob &job);
	void emitSaveFailed(ReceiveJob &job);
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

	QHash< QByteArray, std::shared_ptr< SendJob > > m_sendJobs;
	QHash< QByteArray, std::shared_ptr< ReceiveJob > > m_receiveJobs;
	QHash< unsigned int, PendingHandshake > m_pendingHandshakes;   // responder side, pre-verification
	quint64 m_nextJobNonce = 0;
};

} // namespace PQFT

Q_DECLARE_METATYPE(PQFT::FTTransferInfo)

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERENGINE_H_
