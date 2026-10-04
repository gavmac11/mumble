// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferEngine.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/Argon2Wrap.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/Merkle.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FTMessages.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace PQFT {

namespace {
constexpr int EarlyChunkBufferMax = 8;

QByteArray blobForEarlyChunk(quint64 index, const QByteArray &ciphertext) {
	return uint64be(index) + ciphertext;
}

bool earlyChunkSplit(const QByteArray &blob, quint64 &index, QByteArray &ciphertext) {
	if (blob.size() < 8)
		return false;
	index = 0;
	for (int i = 0; i < 8; ++i) {
		index = (index << 8) | static_cast< unsigned char >(blob.at(i));
	}
	ciphertext = blob.mid(8);
	return true;
}

const QByteArray canonicalEmptyMap = QByteArray::fromHex("a0");
} // namespace

FileTransferEngine::FileTransferEngine(QObject *parent) : QObject(parent) { }

FileTransferEngine::~FileTransferEngine() {
	for (auto &job : m_sendJobs) {
		cleanupSend(*job, true);
	}
	for (auto &job : m_receiveJobs) {
		cleanupReceive(*job);
	}
}

void FileTransferEngine::setTransport(TransportControl control, TransportChunk chunk) {
	m_transportControl = std::move(control);
	m_transportChunk   = std::move(chunk);
}

void FileTransferEngine::setPinLookup(PinLookup lookup) {
	m_pinLookup = std::move(lookup);
}

void FileTransferEngine::setIdentity(QByteArray identityPublicKey, IdentitySign sign) {
	m_identityPk = std::move(identityPublicKey);
	m_sign		 = std::move(sign);
}

void FileTransferEngine::setConfig(const Config &config) {
	m_config = config;
}

QByteArray FileTransferEngine::preManifestKey(unsigned int peerSession) {
	return QByteArray("\x01", 1) + uint32be(peerSession);
}

// ---------------------------------------------------------------------------
// Inbound control

void FileTransferEngine::onControlMessage(unsigned int actorSession, const QByteArray &payload) {
	// Established/pending send sessions with this peer
	for (auto &jobIt : m_sendJobs) {
		SendJob &job = *jobIt;
		for (SendPeer &peer : job.peers) {
			if (peer.session == actorSession && peer.session_ && !peer.failed) {
				const auto st = peer.session_->state();
				if (st == FileTransferSession::State::AwaitingM2
					|| st == FileTransferSession::State::AwaitingM4) {
					processControlForSend(job, peer, payload);
					return;
				}
			}
		}
	}
	// Receive sessions with this peer. Before the handshake completes the
	// payload is an M3 handshake frame; afterwards it is an AEAD control
	// record.
	for (auto &jobIt : m_receiveJobs) {
		if (jobIt->peerSession == actorSession && jobIt->session_) {
			if (jobIt->session_->state() == FileTransferSession::State::AwaitingM3) {
				quint8 frameType = 0;
				if (FTFrame::decodeHeader(payload, frameType) && frameType == FTFrame::TypeM3
					&& jobIt->session_->processM3(payload)) {
					const QByteArray m4 = jobIt->session_->buildM4();
					if (!m4.isEmpty() && m_transportControl) {
						m_transportControl(actorSession, m4);
					} else {
						updateReceiveState(*jobIt, FTTransferInfo::State::Failed,
										   genericDecryptionError());
						cleanupReceive(*jobIt);
					}
				} else {
					updateReceiveState(*jobIt, FTTransferInfo::State::Failed,
									   genericDecryptionError());
					cleanupReceive(*jobIt);
				}
				return;
			}
			processControlForReceive(jobIt, payload);
			return;
		}
	}

	handleIncomingM1(actorSession, payload);
}

void FileTransferEngine::processControlForSend(SendJob &job, SendPeer &peer,
											   const QByteArray &payload) {
	quint8 type = 0;
	if (!FTFrame::decodeHeader(payload, type)) {
		return;
	}

	switch (peer.session_->state()) {
		case FileTransferSession::State::AwaitingM2:
			if (type == FTFrame::TypeM2 && peer.session_->processM2(payload)) {
				const QByteArray m3 = peer.session_->buildM3();
				if (!m3.isEmpty() && m_transportControl) {
					m_transportControl(peer.session, m3);
				} else {
					peer.failed = true;
				}
			} else {
				peer.failed = true;
			}
			break;
		case FileTransferSession::State::AwaitingM4:
			if (type == FTFrame::TypeM4 && peer.session_->processM4(payload)) {
				peer.established = true;
				if (peer.pinnedFingerprint.isEmpty()) {
					// First use: the presented (signature-verified) identity
					// becomes the pin; surface it for the safety-number flow.
					const QByteArray peerFp = peer.session_->peerFingerprint();
					emit firstContact(peer.session, peerFp,
									  safetyNumber(identityFingerprint(m_identityPk), peerFp),
									  job.transferId);
				}
			} else {
				peer.failed = true;
			}
			break;
		default:
			break;
	}

	maybeStartHandshakePhase2(job);
}

void FileTransferEngine::processControlForReceive(const std::shared_ptr< ReceiveJob > &jobPtr,
												  const QByteArray &payload) {
	ReceiveJob &job = *jobPtr;

	quint8 type			 = 0;
	QByteArray body;
	if (!job.session_->openControl(type, body, payload)) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(job);
		return;
	}

	if (type == FTFrame::TypeManifest && !job.haveManifest) {
		auto verify = [&job](const QByteArray &message, const QByteArray &signature,
							 const QByteArray &context) {
			SigMLDSA65 sig;
			return sig.verify(job.session_->peerIdentityKey(), message, signature, context);
		};
		FTManifest manifest;
		if (!parseAndVerifyManifest(manifest, body, job.session_->peerFingerprint(),
									m_config.maxReceiveSize, verify)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(job);
			return;
		}

		job.manifest	  = manifest;
		job.haveManifest  = true;
		job.transferId	  = manifest.transferId;
		job.fileSize	  = manifest.fileSize;
		job.fileName	  = manifest.fileName;
		job.mimeType	  = manifest.mimeType;
		job.passwordMode  = manifest.passwordMode;
		job.bytesDone	  = 0;
		// Re-key under the real transfer id
		m_receiveJobs.insert(job.transferId, jobPtr);
		m_receiveJobs.remove(preManifestKey(job.peerSession));

		job.tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/mumble-ft/"
					  + QString::fromLatin1(manifest.transferId.toHex());
		QDir().mkpath(job.tempDir);
		job.tempFile = job.tempDir + "/content.bin";
		{
			QFile temp(job.tempFile);
			if (!temp.open(QIODevice::ReadWrite | QIODevice::Truncate)
				|| !temp.resize(static_cast< qint64 >(manifest.fileSize))) {
				updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
				cleanupReceive(job);
				return;
			}
		}
		job.receivedBits = QByteArray(static_cast< qsizetype >((manifest.chunkCount + 7) / 8), '\0');
		job.leafHashes.resize(static_cast< int >(manifest.chunkCount));
		job.idleTimer->start(m_config.receiveIdleTimeoutMSecs);

		if (manifest.passwordMode) {
			job.waitingPassword = true;
			updateReceiveState(job, FTTransferInfo::State::WaitingPassword);
			emit passwordRequired(manifest.transferId);
			return;
		}

		const QByteArray sessionKek = job.session_->deriveSessionKek(manifest.transferId);
		const QByteArray wrapNonce1  = job.session_->deriveWrapNonce1(manifest.transferId);
		QByteArray fileKey;
		if (!unwrapFileKeySessionLayer(fileKey, manifest.wrappedFileKey, sessionKek, wrapNonce1,
									   manifest.transferId, false, manifest.fpA, manifest.fpB)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(job);
			return;
		}
		job.fileKey = fileKey;
		updateReceiveState(job, FTTransferInfo::State::Transferring);
		drainEarlyChunks(job);
		return;
	}

	if (type == FTFrame::TypeComplete) {
		tryCompleteReceive(job);
	} else if (type == FTFrame::TypeAbort) {
		updateReceiveState(job, FTTransferInfo::State::Aborted);
		cleanupReceive(job);
	}
}

void FileTransferEngine::drainEarlyChunks(ReceiveJob &job) {
	const QVector< QByteArray > early = std::move(job.earlyChunks);
	job.earlyChunks.clear();
	for (const QByteArray &blob : early) {
		quint64 index = 0;
		QByteArray ciphertext;
		if (earlyChunkSplit(blob, index, ciphertext)) {
			feedReceiveChunk(job, index, ciphertext);
		}
	}
}

void FileTransferEngine::handleIncomingM1(unsigned int actorSession, const QByteArray &payload) {
	quint8 type = 0;
	if (!FTFrame::decodeHeader(payload, type) || type != FTFrame::TypeM1) {
		return;
	}

	const QByteArray peerPk = extractM1IdentityKey(payload);
	if (peerPk.isEmpty()) {
		return; // malformed: ignore
	}
	const QByteArray peerFp = identityFingerprint(peerPk);
	const QByteArray pinned = m_pinLookup ? m_pinLookup(actorSession) : QByteArray();

	if (!pinned.isEmpty() && pinned != peerFp) {
		// TOFU change: hard block with a loud warning (§5); nothing is answered
		emit peerBlocked(actorSession);
		return;
	}

	if (pinned.isEmpty()) {
		// First contact: park the handshake until the user verifies the
		// safety number (mandatory out-of-band verification, §5).
		if (m_pendingHandshakes.contains(actorSession)) {
			return;
		}
		PendingHandshake pending;
		pending.peerSession	   = actorSession;
		pending.peerFingerprint = peerFp;
		pending.m1Frame		   = payload;
		pending.timeout		   = new QTimer(this);
		pending.timeout->setSingleShot(true);
		connect(pending.timeout, &QTimer::timeout, this,
				[this, actorSession]() { m_pendingHandshakes.remove(actorSession); });
		pending.timeout->start(60'000);
		m_pendingHandshakes.insert(actorSession, std::move(pending));

		FTTransferInfo info;
		info.transferId  = QByteArray();
		info.peerSession = actorSession;
		info.incoming	= true;
		info.state		 = FTTransferInfo::State::VerifyingIdentity;
		emitInfo(info);
		emit firstContact(actorSession, peerFp,
						  safetyNumber(identityFingerprint(m_identityPk), peerFp), QByteArray());
		return;
	}

	startResponder(actorSession, payload, peerFp);
}

void FileTransferEngine::startResponder(unsigned int actorSession, const QByteArray &m1Frame,
										 const QByteArray &peerFingerprint) {
	if (m_receiveJobs.contains(preManifestKey(actorSession))) {
		return; // already handshaking with this peer
	}

	auto job			= std::make_shared< ReceiveJob >();
	job->peerSession  = actorSession;
	job->idleTimer	= std::make_unique< QTimer >(this);
	job->idleTimer->setSingleShot(true);
	connect(job->idleTimer.get(), &QTimer::timeout, this, [this, actorSession]() {
		// Fail whatever receive job belongs to this peer on idle
		for (auto &it : m_receiveJobs) {
			if (it->peerSession == actorSession) {
				updateReceiveState(*it, FTTransferInfo::State::Failed, tr("transfer timed out"));
				cleanupReceive(*it);
				return;
			}
		}
	});

	SessionIdentity identity;
	identity.publicKey = m_identityPk;
	identity.sign		= m_sign;
	job->session_ = std::unique_ptr< FileTransferSession >(
		new FileTransferSession(FileTransferSession::Role::Responder, std::move(identity), peerFingerprint));
	if (job->session_->processM1(m1Frame)) {
		const QByteArray m2 = job->session_->buildM2();
		if (!m2.isEmpty() && m_transportControl) {
			m_receiveJobs.insert(preManifestKey(actorSession), job);
			job->idleTimer->start(m_config.receiveIdleTimeoutMSecs);
			m_transportControl(actorSession, m2);
			return;
		}
	}
	// Handshake refused; drop silently (the initiator will time out)
}

void FileTransferEngine::onDataMessage(unsigned int actorSession, const QByteArray &transferId,
									   quint64 chunkIndex, quint64 chunkCountHint,
									   const QByteArray &data) {
	Q_UNUSED(chunkCountHint);

	std::shared_ptr< ReceiveJob > job = findReceiveByPeer(actorSession, transferId);
	if (!job) {
		return;
	}
	job->idleTimer->start(m_config.receiveIdleTimeoutMSecs);

	if (!job->haveManifest) {
		if (job->earlyChunks.size() < EarlyChunkBufferMax) {
			job->earlyChunks.append(blobForEarlyChunk(chunkIndex, data));
		}
		return;
	}

	feedReceiveChunk(*job, chunkIndex, data);
}

void FileTransferEngine::feedReceiveChunk(ReceiveJob &job, quint64 index, const QByteArray &ciphertext) {
	if (job.waitingPassword) {
		if (job.earlyChunks.size() < EarlyChunkBufferMax) {
			job.earlyChunks.append(blobForEarlyChunk(index, ciphertext));
		}
		return;
	}
	if (index >= job.manifest.chunkCount || job.fileKey.isEmpty()
		|| job.lastState == FTTransferInfo::State::Ready
		|| job.lastState == FTTransferInfo::State::Saved
		|| job.lastState == FTTransferInfo::State::Failed
		|| job.lastState == FTTransferInfo::State::Aborted) {
		return;
	}

	const int byteIndex = static_cast< int >(index / 8);
	const int bitMask   = 1 << static_cast< int >(index % 8);
	if (job.receivedBits.at(byteIndex) & bitMask) {
		// Duplicate delivery is fatal by protocol (§9) — a relay has no
		// legitimate reason to duplicate on a reliable transport.
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(job);
		return;
	}

	QByteArray plaintext;
	if (!decryptChunk(plaintext, job.fileKey, job.manifest.transferId, job.manifest.transferDigest(),
					  index, job.manifest.chunkCount, ciphertext)) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(job);
		return;
	}

	{
		QFile file(job.tempFile);
		const qint64 offset = static_cast< qint64 >(index) * job.manifest.chunkSize;
		if (!file.open(QIODevice::ReadWrite) || !file.seek(offset)
			|| file.write(plaintext) != plaintext.size()) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(job);
			return;
		}
	}

	job.leafHashes[static_cast< int >(index)] = merkleChunkHash(plaintext);
	job.receivedBits[byteIndex] = static_cast< char >(job.receivedBits.at(byteIndex) | bitMask);
	++job.receivedCount;
	job.bytesDone += static_cast< quint64 >(plaintext.size());
	if (job.bytesDone - job.lastProgressBytes >= 1024 * 1024) {
		job.lastProgressBytes = job.bytesDone;
		updateReceiveState(job, FTTransferInfo::State::Transferring);
	}

	if (job.receivedCount == job.manifest.chunkCount) {
		tryCompleteReceive(job);
	}
}

bool FileTransferEngine::tryCompleteReceive(ReceiveJob &job) {
	if (!job.haveManifest || job.waitingPassword || job.receivedCount != job.manifest.chunkCount) {
		return false;
	}

	if (merkleRoot(job.leafHashes) != job.manifest.merkleRoot) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(job);
		return false;
	}

	zeroize(job.fileKey);
	updateReceiveState(job, FTTransferInfo::State::Ready);
	return true;
}

// ---------------------------------------------------------------------------
// Sending

quint64 FileTransferEngine::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									  QByteArray password, const QSet< unsigned int > &recipients) {
	QFileInfo fileInfo(filePath);
	if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable() || fileInfo.size() <= 0) {
		zeroize(password);
		return 0;
	}

	auto job			= std::make_shared< SendJob >();
	job->transferId		= randomBytes(TransferIdSize);
	job->sourcePath		= fileInfo.absoluteFilePath();
	job->fileName		= fileInfo.fileName();
	job->mimeType		= mimeType.isEmpty() ? QStringLiteral("application/octet-stream") : mimeType;
	job->passwordMode	= passwordMode;
	job->password		= passwordMode ? password : QByteArray();
	job->fileSize		= static_cast< quint64 >(fileInfo.size());
	job->effectiveChunkSize = qBound< quint32 >(static_cast< quint32 >(MinChunkSize), m_config.chunkSize,
												static_cast< quint32 >(MaxChunkSize));
	job->file			= new QFile(fileInfo.absoluteFilePath());
	if (!job->file->open(QIODevice::ReadOnly)) {
		delete job->file;
		zeroize(password);
		return 0;
	}
	const quint32 chunkSize  = job->effectiveChunkSize;
	const quint64 chunkCount = (job->fileSize + chunkSize - 1) / chunkSize;

	// One hashing pass: chunk digests -> Merkle root
	QVector< QByteArray > digests;
	digests.reserve(static_cast< int >(chunkCount));
	while (!job->file->atEnd()) {
		const QByteArray chunk = job->file->read(chunkSize);
		if (chunk.isEmpty()) {
			break;
		}
		digests.append(merkleChunkHash(chunk));
	}
	if (static_cast< quint64 >(digests.size()) != chunkCount) {
		delete job->file;
		zeroize(password);
		return 0;
	}
	job->merkleRoot = merkleRoot(digests);
	job->chunkCount = chunkCount;
	job->fileKey	= randomBytes(KeySize);

	// transfer digest over the recipient-independent manifest fields
	// (must match FTManifest::transferDigest() on the receiver exactly)
	{
		FTManifest m;
		m.transferId   = job->transferId;
		m.fpA		   = identityFingerprint(m_identityPk);
		m.fileName	   = job->fileName;
		m.mimeType	   = job->mimeType;
		m.fileSize	   = job->fileSize;
		m.chunkSize	   = chunkSize;
		m.chunkCount   = chunkCount;
		m.merkleRoot   = job->merkleRoot;
		m.passwordMode = job->passwordMode;
		job->transferDigest = m.transferDigest();
	}

	// Per-recipient sessions (peers must be pinned already)
	bool anyPeer = false;
	for (unsigned int session : recipients) {
		const QByteArray pinned = m_pinLookup ? m_pinLookup(session) : QByteArray();
		SendPeer peer;
		peer.session  = session;
		peer.pinnedFingerprint = pinned;
		SessionIdentity identity;
		identity.publicKey = m_identityPk;
		identity.sign		= m_sign;
		peer.session_ = new FileTransferSession(FileTransferSession::Role::Initiator, std::move(identity),
												 pinned);
		job->peers.append(std::move(peer));
		anyPeer = true;
	}
	if (!anyPeer) {
		delete job->file;
		zeroize(password);
		return 0;
	}

	const QByteArray tid = job->transferId;
	job->handshakeTimer  = std::make_unique< QTimer >(this);
	job->handshakeTimer->setSingleShot(true);
	connect(job->handshakeTimer.get(), &QTimer::timeout, this, [this, tid]() {
		auto it = m_sendJobs.find(tid);
		if (it != m_sendJobs.end()) {
			maybeStartHandshakePhase2(*it->get());
		}
	});
	job->handshakeTimer->start(m_config.handshakeTimeoutMSecs);

	m_sendJobs.insert(job->transferId, job);

	for (SendPeer &peer : job->peers) {
		const QByteArray m1 = peer.session_->buildM1();
		if (!m1.isEmpty() && m_transportControl) {
			m_transportControl(peer.session, m1);
		}
	}

	FTTransferInfo info;
	info.transferId   = job->transferId;
	info.peerSession  = job->peers.first().session;
	info.incoming	  = false;
	info.fileName	  = job->fileName;
	info.mimeType	  = job->mimeType;
	info.fileSize	  = job->fileSize;
	info.passwordMode = job->passwordMode;
	info.state		  = FTTransferInfo::State::Handshaking;
	emitInfo(info);

	zeroize(password);
	return job->fileSize;
}

void FileTransferEngine::maybeStartHandshakePhase2(SendJob &job) {
	bool pending = false;
	for (const SendPeer &peer : job.peers) {
		if (!peer.established && !peer.failed) {
			const auto st = peer.session_ ? peer.session_->state()
										 : FileTransferSession::State::Failed;
			if (st == FileTransferSession::State::AwaitingM2
				|| st == FileTransferSession::State::AwaitingM4
				|| st == FileTransferSession::State::Created) {
				pending = true;
			}
		}
	}
	if (pending && job.handshakeTimer && job.handshakeTimer->isActive()) {
		return;
	}

	bool anyEstablished = false;
	for (SendPeer &peer : job.peers) {
		if (!peer.established) {
			peer.failed = true;
		} else {
			anyEstablished = true;
		}
	}
	if (!anyEstablished) {
		finishSend(job, false, tr("No recipient could be reached"));
		return;
	}
	if (!job.manifestSent) {
		buildAndSendManifests(job);
	}
}

void FileTransferEngine::buildAndSendManifests(SendJob &job) {
	// Password-layer material is shared by every recipient of the transfer
	SecureBytes pwKey;
	QByteArray salt;
	if (job.passwordMode) {
		salt = randomBytes(Argon2SaltSize);
		QByteArray pw = job.password;
		if (!argon2idDerive(pwKey, pw, salt, Argon2Params())) {
			zeroize(job.password);
			finishSend(job, false, genericDecryptionError());
			return;
		}
		zeroize(job.password);
	}

	const QByteArray fpA = identityFingerprint(m_identityPk);

	for (SendPeer &peer : job.peers) {
		if (!peer.established || !peer.session_) {
			continue;
		}
		const QByteArray sessionKek = peer.session_->deriveSessionKek(job.transferId);
		const QByteArray wrapNonce1  = peer.session_->deriveWrapNonce1(job.transferId);
		const QByteArray fpB		  = peer.session_->peerFingerprint();

		QByteArray layer1;
		if (!wrapFileKeySessionLayer(layer1, job.fileKey, sessionKek, wrapNonce1, job.transferId,
									 job.passwordMode, fpA, fpB)) {
			peer.failed = true;
			continue;
		}

		FTManifest manifest;
		manifest.transferId	 = job.transferId;
		manifest.fpA			 = fpA;
		manifest.fpB			 = fpB;
		manifest.fileName		 = job.fileName;
		manifest.mimeType		 = job.mimeType;
		manifest.fileSize		 = job.fileSize;
		manifest.chunkSize		 = job.effectiveChunkSize;
		manifest.chunkCount	 = job.chunkCount;
		manifest.merkleRoot	 = job.merkleRoot;
		manifest.passwordMode   = job.passwordMode;
		manifest.createdAtUnix  = static_cast< quint64 >(QDateTime::currentSecsSinceEpoch());

		if (job.passwordMode) {
			manifest.argon2	 = Argon2Params();
			manifest.argon2Salt = salt;
			const QByteArray pwWrapKey =
				passwordWrapKey(pwKey.toByteArray(), job.transferId, fpA, fpB);
			QByteArray layer2;
			if (!wrapLayer1WithPassword(layer2, layer1, pwWrapKey, job.transferId, true, fpA, fpB,
										Argon2Params(), salt)) {
				peer.failed = true;
				continue;
			}
			manifest.wrappedFileKey = layer2;
		} else {
			manifest.wrappedFileKey = layer1;
		}

		if (!signManifest(manifest, m_sign)) {
			peer.failed = true;
			continue;
		}

		const QByteArray frame =
			peer.session_->sealControl(FTFrame::TypeManifest, encodeManifest(manifest));
		if (frame.isEmpty() || !m_transportControl) {
			peer.failed = true;
			continue;
		}
		m_transportControl(peer.session, frame);
	}
	pwKey.clear();

	bool noneSucceeded = true;
	for (const SendPeer &peer : job.peers) {
		if (peer.established && !peer.failed) {
			noneSucceeded = false;
		}
	}
	if (noneSucceeded) {
		finishSend(job, false, genericDecryptionError());
		return;
	}

	job.manifestSent = true;
	updateSendState(job, FTTransferInfo::State::Transferring);

	job.paceTimer = std::make_unique< QTimer >(this);
	connect(job.paceTimer.get(), &QTimer::timeout, this, [this, tid = job.transferId]() {
		auto it = m_sendJobs.find(tid);
		if (it == m_sendJobs.end()) {
			return;
		}
		SendJob &j = *it->get();
		const qint64 budget =
			m_config.sendRateBytesPerSecond > 0
				? qMax< qint64 >(m_config.sendRateBytesPerSecond / 20,
								 static_cast< qint64 >(j.effectiveChunkSize))
				: std::numeric_limits< qint64 >::max();
		sendNextChunks(j, budget);
	});
	job.paceTimer->start(50);
}

void FileTransferEngine::sendNextChunks(SendJob &job, qint64 budgetBytes) {
	if (job.lastState == FTTransferInfo::State::Aborted
		|| job.lastState == FTTransferInfo::State::Failed) {
		return;
	}
	const quint32 chunkSize = job.effectiveChunkSize;

	while (budgetBytes > 0 && job.nextChunkIndex < job.chunkCount) {
		const qint64 offset = static_cast< qint64 >(job.nextChunkIndex) * chunkSize;
		if (!job.file->seek(offset)) {
			finishSend(job, false, tr("Read error"));
			return;
		}
		const QByteArray chunk = job.file->read(chunkSize);
		if (chunk.isEmpty()) {
			finishSend(job, false, tr("Read error"));
			return;
		}

		QByteArray ciphertext;
		if (!encryptChunk(ciphertext, job.fileKey, job.transferId, job.transferDigest,
						  job.nextChunkIndex, job.chunkCount, chunk)) {
			finishSend(job, false, tr("Encryption error"));
			return;
		}
		if (m_transportChunk) {
			m_transportChunk(job.transferId, job.nextChunkIndex, job.chunkCount, ciphertext);
		}

		budgetBytes -= chunk.size();
		job.bytesDone += static_cast< quint64 >(chunk.size());
		++job.nextChunkIndex;
	}

	if (job.bytesDone - job.lastProgressBytes >= 1024 * 1024
		|| job.nextChunkIndex >= job.chunkCount) {
		job.lastProgressBytes = job.bytesDone;
		updateSendState(job, FTTransferInfo::State::Transferring);
	}

	if (job.nextChunkIndex >= job.chunkCount) {
		for (SendPeer &peer : job.peers) {
			if (peer.established && peer.session_) {
				const QByteArray frame =
					peer.session_->sealControl(FTFrame::TypeComplete, canonicalEmptyMap);
				if (!frame.isEmpty() && m_transportControl) {
					m_transportControl(peer.session, frame);
				}
			}
		}
		finishSend(job, true, QString());
	}
}

void FileTransferEngine::finishSend(SendJob &job, bool success, const QString &error) {
	updateSendState(job, success ? FTTransferInfo::State::Saved : FTTransferInfo::State::Failed, error);
	cleanupSend(job, true);
}

void FileTransferEngine::cleanupSend(SendJob &job, bool keepCard) {
	Q_UNUSED(keepCard);
	if (job.file) {
		job.file->close();
		delete job.file;
		job.file = nullptr;
	}
	zeroize(job.fileKey);
	zeroize(job.password);
	job.paceTimer.reset();
	job.handshakeTimer.reset();
	for (SendPeer &peer : job.peers) {
		delete peer.session_;
		peer.session_ = nullptr;
	}
	job.peers.clear();
	m_sendJobs.remove(job.transferId);
}

void FileTransferEngine::cleanupReceive(ReceiveJob &job) {
	// Temp files survive only for Ready transfers (until saved); every other
	// outcome removes all partial output (§13).
	if (job.lastState != FTTransferInfo::State::Ready && !job.tempDir.isEmpty()) {
		QDir(job.tempDir).removeRecursively();
	}
	zeroize(job.fileKey);
	job.idleTimer.reset();
	job.session_.reset();
	if (!job.transferId.isEmpty()) {
		m_receiveJobs.remove(job.transferId);
	}
	m_receiveJobs.remove(preManifestKey(job.peerSession));
}

// ---------------------------------------------------------------------------
// Manager requests

void FileTransferEngine::abortTransfer(const QByteArray &transferId) {
	auto sendIt = m_sendJobs.find(transferId);
	if (sendIt != m_sendJobs.end()) {
		SendJob &job = *sendIt->get();
		for (SendPeer &peer : job.peers) {
			if (peer.established && peer.session_ && m_transportControl) {
				const QByteArray frame =
					peer.session_->sealControl(FTFrame::TypeAbort, canonicalEmptyMap);
				if (!frame.isEmpty()) {
					m_transportControl(peer.session, frame);
				}
			}
		}
		updateSendState(job, FTTransferInfo::State::Aborted);
		cleanupSend(job, true);
		return;
	}
	auto recvIt = m_receiveJobs.find(transferId);
	if (recvIt != m_receiveJobs.end()) {
		updateReceiveState(*recvIt->get(), FTTransferInfo::State::Aborted);
		cleanupReceive(*recvIt->get());
	}
}

void FileTransferEngine::saveTransferAs(const QByteArray &transferId, const QString &targetPath) {
	auto it = m_receiveJobs.find(transferId);
	if (it == m_receiveJobs.end() || it->get()->lastState != FTTransferInfo::State::Ready) {
		return;
	}
	ReceiveJob &job = *it->get();

	const QFileInfo targetInfo(targetPath);
	if (!targetInfo.dir().mkpath(".")) {
		emitSaveFailed(job);
		return;
	}

	// Atomic rename when possible; cross-device falls back to copy+rename
	if (QFile::rename(job.tempFile, targetPath)) {
		emitSaveDone(job);
		return;
	}
	const QString partPath = targetPath + ".part";
	if (QFile::copy(job.tempFile, partPath) && QFile::rename(partPath, targetPath)) {
		emitSaveDone(job);
		return;
	}
	QFile::remove(partPath);
	emitSaveFailed(job);
}

void FileTransferEngine::providePassword(const QByteArray &transferId, QByteArray password) {
	auto it = m_receiveJobs.find(transferId);
	if (it == m_receiveJobs.end() || !it->get()->waitingPassword) {
		zeroize(password);
		return;
	}
	ReceiveJob &job = *it->get();

	{
		QByteArray pw = password;
		SecureBytes pwKey;
		if (!argon2idDerive(pwKey, pw, job.manifest.argon2Salt, *job.manifest.argon2)) {
			zeroize(password);
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(job);
			return;
		}
		zeroize(password);

		const QByteArray pwWrapKey =
			passwordWrapKey(pwKey.toByteArray(), job.transferId, job.manifest.fpA, job.manifest.fpB);
		QByteArray layer1;
		if (!unwrapPasswordLayer(layer1, job.manifest.wrappedFileKey, pwWrapKey, job.transferId,
								 true, job.manifest.fpA, job.manifest.fpB, *job.manifest.argon2,
								 job.manifest.argon2Salt)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(job);
			return;
		}

		const QByteArray sessionKek = job.session_->deriveSessionKek(job.transferId);
		const QByteArray wrapNonce1  = job.session_->deriveWrapNonce1(job.transferId);
		QByteArray fileKey;
		if (!unwrapFileKeySessionLayer(fileKey, layer1, sessionKek, wrapNonce1, job.transferId, true,
									   job.manifest.fpA, job.manifest.fpB)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(job);
			return;
		}
		job.fileKey = fileKey;
	}

	job.waitingPassword = false;
	updateReceiveState(job, FTTransferInfo::State::Transferring);
	drainEarlyChunks(job);
	if (job.receivedCount == job.manifest.chunkCount) {
		tryCompleteReceive(job);
	}
}

void FileTransferEngine::abortAll() {
	const QList< QByteArray > sendKeys = m_sendJobs.keys();
	for (const QByteArray &key : sendKeys) {
		abortTransfer(key);
	}
	const QList< QByteArray > recvKeys = m_receiveJobs.keys();
	for (const QByteArray &key : recvKeys) {
		// Synthetic pre-manifest keys are removed by cleanupReceive too
		abortTransfer(key);
	}
	m_pendingHandshakes.clear();
}

void FileTransferEngine::resolveFirstContact(unsigned int peerSession, bool verified) {
	auto it = m_pendingHandshakes.find(peerSession);
	if (it == m_pendingHandshakes.end()) {
		return;
	}
	PendingHandshake pending = std::move(it.value());
	m_pendingHandshakes.erase(it);

	if (verified) {
		// The manager has pinned the fingerprint; run the M1 now
		handleIncomingM1(peerSession, pending.m1Frame);
	}
	// Declined: nothing was ever answered — the initiator times out
}

// ---------------------------------------------------------------------------
// Helpers

void FileTransferEngine::updateSendState(SendJob &job, FTTransferInfo::State state,
										 const QString &error) {
	job.lastState = state;

	FTTransferInfo info;
	info.transferId   = job.transferId;
	info.peerSession  = job.peers.isEmpty() ? 0 : job.peers.first().session;
	info.incoming	  = false;
	info.fileName	  = job.fileName;
	info.mimeType	  = job.mimeType;
	info.fileSize	  = job.fileSize;
	info.bytesDone	  = job.bytesDone;
	info.passwordMode = job.passwordMode;
	info.state		  = state;
	info.error		  = error;
	emitInfo(info);
}

void FileTransferEngine::updateReceiveState(ReceiveJob &job, FTTransferInfo::State state,
											const QString &error) {
	job.lastState = state;

	FTTransferInfo info;
	info.transferId   = job.transferId.isEmpty() ? job.manifest.transferId : job.transferId;
	info.peerSession  = job.peerSession;
	info.incoming	  = true;
	info.fileName	  = job.fileName;
	info.mimeType	  = job.mimeType;
	info.fileSize	  = job.fileSize;
	info.bytesDone	  = job.bytesDone;
	info.passwordMode = job.passwordMode;
	info.state		  = state;
	info.error		  = error;
	emitInfo(info);
}

void FileTransferEngine::emitInfo(const FTTransferInfo &info) {
	emit transferUpdated(info);
}

void FileTransferEngine::emitSaveDone(ReceiveJob &job) {
	job.lastState = FTTransferInfo::State::Saved;
	FTTransferInfo info;
	info.transferId  = job.transferId;
	info.peerSession = job.peerSession;
	info.incoming	= true;
	info.fileName	= job.fileName;
	info.fileSize	= job.fileSize;
	info.state		 = FTTransferInfo::State::Saved;
	emitInfo(info);
	QDir(job.tempDir).removeRecursively();
	cleanupReceive(job);
}

void FileTransferEngine::emitSaveFailed(ReceiveJob &job) {
	FTTransferInfo info;
	info.transferId  = job.transferId;
	info.peerSession = job.peerSession;
	info.incoming	= true;
	info.fileName	= job.fileName;
	info.fileSize	= job.fileSize;
	info.state		 = FTTransferInfo::State::Ready;
	info.error		 = tr("Could not save the file");
	emitInfo(info);
}

std::shared_ptr< FileTransferEngine::ReceiveJob > FileTransferEngine::findReceiveByPeer(
	unsigned int peerSession, const QByteArray &transferId) {
	auto it = m_receiveJobs.find(transferId);
	if (it != m_receiveJobs.end() && it->get()->peerSession == peerSession) {
		return *it;
	}
	// Pre-manifest jobs live under the synthetic key
	auto pre = m_receiveJobs.find(preManifestKey(peerSession));
	if (pre != m_receiveJobs.end()) {
		return *pre;
	}
	return nullptr;
}

} // namespace PQFT
