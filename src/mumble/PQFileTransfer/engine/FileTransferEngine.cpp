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
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>

#include <cerrno>
#include <filesystem>

#ifdef Q_OS_WIN
#	include <io.h>
#else
#	include <fcntl.h>
#	include <unistd.h>
#endif

namespace PQFT {

namespace {
constexpr int EarlyChunkBufferMax = 8;

bool syncSaveFile(QFileDevice &file) {
	if (!file.flush() || file.handle() < 0)
		return false;
#ifdef Q_OS_WIN
	return ::_commit(file.handle()) == 0;
#else
	int result;
	do {
		result = ::fsync(file.handle());
	} while (result != 0 && errno == EINTR);
	return result == 0;
#endif
}

bool syncSavePublication(const QString &targetPath) {
#ifdef Q_OS_WIN
	// Check a flush on the published file too. This does not claim Windows
	// directory-journal or physical power-loss qualification.
	QFile target(targetPath);
	return target.open(QIODevice::ReadWrite) && syncSaveFile(target);
#else
	const QByteArray directory = QFile::encodeName(QFileInfo(targetPath).absolutePath());
	const int fd               = ::open(directory.constData(), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return false;
	int result;
	do {
		result = ::fsync(fd);
	} while (result != 0 && errno == EINTR);
	::close(fd);
	return result == 0;
#endif
}

bool shutdownRequested() {
	return QThread::currentThread()->isInterruptionRequested();
}

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

// Spool record: "u32be payload length" || "u64be index" || ciphertext. The
// length prefix makes the on-disk stream parseable.
QByteArray spoolRecord(quint64 index, const QByteArray &ciphertext) {
	const quint32 payloadLength = static_cast< quint32 >(8 + ciphertext.size());
	QByteArray record(4, Qt::Uninitialized);
	record[0] = static_cast< char >((payloadLength >> 24) & 0xff);
	record[1] = static_cast< char >((payloadLength >> 16) & 0xff);
	record[2] = static_cast< char >((payloadLength >> 8) & 0xff);
	record[3] = static_cast< char >(payloadLength & 0xff);
	return record + blobForEarlyChunk(index, ciphertext);
}

const QByteArray canonicalEmptyMap = QByteArray::fromHex("a0");
} // namespace

FileTransferEngine::FileTransferEngine(QObject *parent) : QObject(parent) {
	m_syncSaveFile        = syncSaveFile;
	m_syncSavePublication = syncSavePublication;
	m_sendPaceTimer = std::make_unique< QTimer >(this);
	connect(m_sendPaceTimer.get(), &QTimer::timeout, this, &FileTransferEngine::paceSends);
}

FileTransferEngine::~FileTransferEngine() {
	// Take owning copies first: cleanup*() removes the jobs from the maps,
	// which would invalidate the iterators mid-loop.
	const QList< std::shared_ptr< SendJob > > sendJobs = m_sendJobs.values();
	for (const auto &job : sendJobs) {
		cleanupSend(*job, true);
	}
	const QList< std::shared_ptr< ReceiveJob > > recvJobs = m_receiveJobs.values();
	for (const auto &job : recvJobs) {
		cleanupReceive(job, false);
	}
}

void FileTransferEngine::setTransport(TransportControl control, TransportChunk chunk) {
	m_transportControl = std::move(control);
	m_transportChunk   = std::move(chunk);
}

void FileTransferEngine::setPinLookup(PinLookup lookup) {
	m_pinLookup = std::move(lookup);
}

void FileTransferEngine::setPeerNameLookup(std::function< QString(unsigned int) > lookup) {
	m_peerNameLookup = std::move(lookup);
}

void FileTransferEngine::setIdentity(QByteArray identityPublicKey, IdentitySign sign) {
	m_identityPk = std::move(identityPublicKey);
	m_sign		 = std::move(sign);
}

void FileTransferEngine::setConfig(const Config &config) {
	const bool rateChanged = m_config.sendRateBytesPerSecond != config.sendRateBytesPerSecond;
	m_config = config;
	// Pin-cache refreshes and new sends also apply configuration. Preserve
	// earned credit unless the rate itself changes, so these cannot starve sends.
	if (rateChanged) {
		m_sendCreditMilliBytes = 0;
		m_sendCreditLastNSecs  = 0;
		m_sendCreditRemainder  = 0;
		if (m_sendPaceTimer->isActive())
			m_sendPaceClock.start();
	}
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

	// Receive side. Several jobs can coexist with one peer (a finished but
	// unsaved transfer next to a new one, say), and records are opaque
	// per-session frames, so route deliberately:
	//   - a plaintext M1 always starts a NEW transfer (an established
	//     session's keys could never open it);
	//   - an M3 belongs to the single pre-manifest handshake in flight;
	//   - established AEAD records are probed non-destructively against
	//     every ACTIVE job of that peer and delivered to the one that
	//     authenticates them. Terminal jobs (Ready/Saved/...) are never
	//     consulted, and a record no session can open is dropped — a stray
	//     frame must not kill unrelated transfers of the same peer.
	quint8 headerType = 0;
	const bool isM1 =
		FTFrame::decodeHeader(payload, headerType) && headerType == FTFrame::TypeM1;

	if (!isM1) {
		std::shared_ptr< ReceiveJob > awaitingM3;
		for (auto &jobIt : m_receiveJobs) {
			if (jobIt->peerSession != actorSession || !jobIt->session_) {
				continue;
			}
			if (jobIt->session_->state() == FileTransferSession::State::AwaitingM3) {
				awaitingM3 = jobIt;   // at most one handshake in flight per peer
				continue;
			}
			if (jobIt->session_->state() != FileTransferSession::State::Established
				|| jobIt->lastState == FTTransferInfo::State::Ready
				|| jobIt->lastState == FTTransferInfo::State::Saved
				|| jobIt->lastState == FTTransferInfo::State::Failed
				|| jobIt->lastState == FTTransferInfo::State::Aborted) {
				continue;
			}
			quint8 type = 0;
			QByteArray body;
			if (jobIt->session_->tryOpenControl(type, body, payload)) {
				processControlForReceive(jobIt, type, body);
				return;
			}
		}
		if (awaitingM3) {
			quint8 frameType = 0;
			if (FTFrame::decodeHeader(payload, frameType) && frameType == FTFrame::TypeM3
				&& awaitingM3->session_->processM3(payload)) {
				const QByteArray m4 = awaitingM3->session_->buildM4();
				if (!m4.isEmpty() && m_transportControl) {
					m_transportControl(actorSession, m4);
				} else {
					updateReceiveState(*awaitingM3, FTTransferInfo::State::Failed,
									   genericDecryptionError());
					cleanupReceive(awaitingM3);
				}
			} else {
				updateReceiveState(*awaitingM3, FTTransferInfo::State::Failed,
								   genericDecryptionError());
				cleanupReceive(awaitingM3);
			}
			return;
		}
		return;   // nothing claims this record: drop it
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
					// The handshake has authenticated the key, so
					// pin-on-observation (the documented sender-side TOFU
					// deviation) is sound here.
					const QByteArray peerFp = peer.session_->peerFingerprint();
					emit firstContact(peer.session, peerFp,
									  safetyNumber(identityFingerprint(m_identityPk), peerFp),
									  job.transferId, true);
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

void FileTransferEngine::processControlForReceive(std::shared_ptr< ReceiveJob > jobPtr,
												  quint8 type, const QByteArray &body) {
	ReceiveJob &job = *jobPtr;

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
			cleanupReceive(jobPtr);
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

		// QTemporaryDir creates an unpredictable owner-only directory atomically,
		// so other local users cannot pre-create a path or redirect plaintext writes.
		job.tempDirectory = std::make_unique< QTemporaryDir >(
			QDir::temp().filePath("mumble-ft-" + QString::fromLatin1(manifest.transferId.toHex()) + "-XXXXXX"));
		if (!job.tempDirectory->isValid()) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(jobPtr);
			return;
		}
		job.tempDir  = job.tempDirectory->path();
		job.tempFile = job.tempDir + "/content.bin";
		{
			QFile temp(job.tempFile);
			if (!temp.open(QIODevice::ReadWrite | QIODevice::Truncate)
				|| !temp.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
				|| !temp.resize(static_cast< qint64 >(manifest.fileSize))) {
				updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
				cleanupReceive(jobPtr);
				return;
			}
		}
		job.receivedBits = QByteArray(static_cast< qsizetype >((manifest.chunkCount + 7) / 8), '\0');
		job.spooledBits  = QByteArray(static_cast< qsizetype >((manifest.chunkCount + 7) / 8), '\0');
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
			cleanupReceive(jobPtr);
			return;
		}
		job.fileKey = fileKey;
		updateReceiveState(job, FTTransferInfo::State::Transferring);
		drainEarlyChunks(jobPtr);
		return;
	}

	if (type == FTFrame::TypeComplete) {
		tryCompleteReceive(jobPtr);
	} else if (type == FTFrame::TypeAbort) {
		updateReceiveState(job, FTTransferInfo::State::Aborted);
		cleanupReceive(jobPtr);
	}
}

void FileTransferEngine::drainEarlyChunks(std::shared_ptr< ReceiveJob > jobPtr) {
	const QVector< ReceiveJob::EarlyChunk > early = std::move(jobPtr->earlyChunks);
	jobPtr->earlyChunks.clear();
	for (const auto &chunk : early) {
		if (shutdownRequested())
			return;
		if (chunk.transferId == jobPtr->manifest.transferId) {
			feedReceiveChunk(jobPtr, chunk.index, chunk.ciphertext);
		}
	}
}

bool FileTransferEngine::openSpool(ReceiveJob &job) {
	job.spoolFile = job.tempDir + "/chunks.spool";
	job.spool     = new QFile(job.spoolFile);
	if (!job.spool->open(QIODevice::WriteOnly | QIODevice::Append)
		|| !job.spool->setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
		delete job.spool;
		job.spool = nullptr;
		return false;
	}
	return true;
}

void FileTransferEngine::drainSpooledChunks(std::shared_ptr< ReceiveJob > jobPtr) {
	ReceiveJob &job = *jobPtr;
	if (!job.spool) {
		return;
	}
	job.spool->close();
	delete job.spool;
	job.spool = nullptr;

	QFile spool(job.spoolFile);
	if (!spool.open(QIODevice::ReadOnly)) {
		return;   // nothing was ever spooled
	}
	while (!shutdownRequested()) {
		QByteArray header = spool.read(4);
		if (header.size() < 4) {
			break;   // clean end (or a truncated spool: the Merkle/AEAD checks
					 // decide the transfer's fate either way)
		}
		quint32 payloadLength = 0;
		for (int i = 0; i < 4; ++i) {
			payloadLength = (payloadLength << 8) | static_cast< unsigned char >(header.at(i));
		}
		if (payloadLength < 8) {
			break;
		}
		const QByteArray payload = spool.read(static_cast< qint64 >(payloadLength));
		if (payload.size() < static_cast< int >(payloadLength)) {
			break;
		}
		quint64 index = 0;
		QByteArray ciphertext;
		if (earlyChunkSplit(payload, index, ciphertext)) {
			feedReceiveChunk(jobPtr, index, ciphertext);
		}
		if (job.lastState == FTTransferInfo::State::Failed
			|| job.lastState == FTTransferInfo::State::Aborted) {
			break;   // the replay killed the job; everything is cleaned up
		}
	}
	spool.close();
	QFile::remove(job.spoolFile);
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
		pending.peerSession     = actorSession;
		pending.peerFingerprint = peerFp;
		pending.m1Frame         = payload;
		pending.timeout         = std::make_unique< QTimer >(this).release();
		pending.timeout->setSingleShot(true);
		QTimer *timer = pending.timeout;
		connect(timer, &QTimer::timeout, this, [this, actorSession, timer]() {
			const auto it = m_pendingHandshakes.find(actorSession);
			if (it != m_pendingHandshakes.end() && it->timeout == timer) {
				m_pendingHandshakes.erase(it);
			}
			timer->deleteLater();
		});
		pending.timeout->start(60'000);
		m_pendingHandshakes.insert(actorSession, std::move(pending));

		FTTransferInfo info;
		info.transferId  = QByteArray();
		info.peerSession = actorSession;
		info.incoming    = true;
		info.state		 = FTTransferInfo::State::VerifyingIdentity;
		emitInfo(info);
		// Plain M1: the fingerprint is an unauthenticated claim until the
		// handshake completes — pin only after the user verifies (§5).
		emit firstContact(actorSession, peerFp,
						  safetyNumber(identityFingerprint(m_identityPk), peerFp), QByteArray(),
						  false);
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
	job->peerName       = m_peerNameLookup ? m_peerNameLookup(actorSession) : QString();
	job->idleTimer	= std::make_unique< QTimer >(this);
	job->idleTimer->setSingleShot(true);
	// Target THIS job, not "the first job of that peer": several transfers
	// (finished or in flight) can coexist with one sender.
	const std::weak_ptr< ReceiveJob > weakJob = job;
	connect(job->idleTimer.get(), &QTimer::timeout, this, [this, weakJob]() {
		const std::shared_ptr< ReceiveJob > idleJob = weakJob.lock();
		if (!idleJob) {
			return;
		}
		updateReceiveState(*idleJob, FTTransferInfo::State::Failed, tr("transfer timed out"));
		cleanupReceive(idleJob);
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

void FileTransferEngine::onDataMessage(unsigned int actorSession, const QByteArray &transferId, quint64 chunkIndex,
									   quint64 chunkCountHint, const QByteArray &data) {
	Q_UNUSED(chunkCountHint);

	if (transferId.size() != TransferIdSize || data.size() < TagSize || data.size() > MaxChunkSize + TagSize)
		return;
	std::shared_ptr< ReceiveJob > job = findReceiveByPeer(actorSession, transferId);
	if (!job) {
		return;
	}
	if (!job->haveManifest) {
		// The sender may be relaying another transfer to the channel. Keep its
		// ID until the authenticated manifest can identify our own chunks, and
		// never extend a handshake timeout for unauthenticated early data.
		if (job->earlyChunks.size() < EarlyChunkBufferMax) {
			job->earlyChunks.append({ transferId, chunkIndex, data });
		}
		return;
	}

	feedReceiveChunk(job, chunkIndex, data);
}

void FileTransferEngine::feedReceiveChunk(std::shared_ptr< ReceiveJob > jobPtr, quint64 index,
										  const QByteArray &ciphertext) {
	ReceiveJob &job = *jobPtr;
	if (job.waitingPassword) {
		// The sender streams on without a readiness acknowledgement, so every
		// chunk must be spooled to disk — a bounded memory buffer would
		// silently drop everything after it filled. A pending password must
		// not suspend the protocol's bounds though: index, exact per-chunk
		// length and duplicates are validated BEFORE anything is written, so a
		// misbehaving relay cannot grow the spool past the manifest-implied
		// size (or keep the transfer alive) by repeating records.
		if (index >= job.manifest.chunkCount) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}
		const quint64 expectedPlain = (index + 1 == job.manifest.chunkCount)
										  ? job.manifest.fileSize - index * job.manifest.chunkSize
										  : job.manifest.chunkSize;
		if (static_cast< quint64 >(ciphertext.size()) != expectedPlain + TagSize) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}
		const int byteIndex = static_cast< int >(index / 8);
		const int bitMask   = 1 << static_cast< int >(index % 8);
		if (job.spooledBits.at(byteIndex) & bitMask) {
			// Duplicate delivery is fatal by protocol (§9) — same policy as the
			// decrypt path, applied at spool time.
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}
		const QByteArray record = spoolRecord(index, ciphertext);
		// Every accepted record is exactly (framing 12 + tag 16 + expected
		// plaintext) bytes and each index at most once, so this cap can only
		// trip if that invariant is broken — enforced anyway to stay fail-closed.
		const quint64 spoolCapacity =
			job.manifest.fileSize + 28 * job.manifest.chunkCount;
		if (!job.spool && !openSpool(job)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(jobPtr);
			return;
		}
		if (job.spoolBytes + static_cast< quint64 >(record.size()) > spoolCapacity) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(jobPtr);
			return;
		}
		if (job.spool->write(record) != record.size()) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(jobPtr);
			return;
		}
		job.idleTimer->start(m_config.receiveIdleTimeoutMSecs);
		job.spoolBytes += static_cast< quint64 >(record.size());
		job.spooledBits[byteIndex] =
			static_cast< char >(job.spooledBits.at(byteIndex) | bitMask);
		return;
	}
	if (index >= job.manifest.chunkCount || job.fileKey.isEmpty()
		|| job.lastState == FTTransferInfo::State::Ready
		|| job.lastState == FTTransferInfo::State::Saved
		|| job.lastState == FTTransferInfo::State::Failed
		|| job.lastState == FTTransferInfo::State::Aborted) {
		return;
	}

	// Every chunk's plaintext length is fully determined by the manifest:
	// chunkSize for all but the final chunk, which carries the remainder.
	// Anything else is corruption (and keeps oversized plaintext out of
	// the preallocated file).
	const quint64 expectedPlain = (index + 1 == job.manifest.chunkCount)
										  ? job.manifest.fileSize - index * job.manifest.chunkSize
										  : job.manifest.chunkSize;
	if (static_cast< quint64 >(ciphertext.size()) != expectedPlain + TagSize) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(jobPtr);
		return;
	}

	const int byteIndex = static_cast< int >(index / 8);
	const int bitMask   = 1 << static_cast< int >(index % 8);
	if (job.receivedBits.at(byteIndex) & bitMask) {
		// Duplicate delivery is fatal by protocol (§9) — a relay has no
		// legitimate reason to duplicate on a reliable transport.
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(jobPtr);
		return;
	}

	QByteArray plaintext;
	if (!decryptChunk(plaintext, job.fileKey, job.manifest.transferId, job.manifest.transferDigest(),
					  index, job.manifest.chunkCount, ciphertext)) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(jobPtr);
		return;
	}

	{
		QFile file(job.tempFile);
		const qint64 offset = static_cast< qint64 >(index) * job.manifest.chunkSize;
		if (!file.open(QIODevice::ReadWrite) || !file.seek(offset)
			|| file.write(plaintext) != plaintext.size()) {
			updateReceiveState(job, FTTransferInfo::State::Failed, tr("storage error"));
			cleanupReceive(jobPtr);
			return;
		}
	}

	job.idleTimer->start(m_config.receiveIdleTimeoutMSecs);
	job.leafHashes[static_cast< int >(index)] = merkleChunkHash(plaintext);
	job.receivedBits[byteIndex] = static_cast< char >(job.receivedBits.at(byteIndex) | bitMask);
	++job.receivedCount;
	job.bytesDone += static_cast< quint64 >(plaintext.size());
	if (job.bytesDone - job.lastProgressBytes >= 1024 * 1024) {
		job.lastProgressBytes = job.bytesDone;
		updateReceiveState(job, FTTransferInfo::State::Transferring);
	}

	if (job.receivedCount == job.manifest.chunkCount) {
		tryCompleteReceive(jobPtr);
	}
}

bool FileTransferEngine::tryCompleteReceive(std::shared_ptr< ReceiveJob > jobPtr) {
	ReceiveJob &job = *jobPtr;
	if (!job.haveManifest || job.waitingPassword || job.receivedCount != job.manifest.chunkCount) {
		return false;
	}

	const QByteArray root = merkleRoot(job.leafHashes, shutdownRequested);
	if (shutdownRequested())
		return false;
	if (root != job.manifest.merkleRoot) {
		updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
		cleanupReceive(jobPtr);
		return false;
	}

	zeroize(job.fileKey);
	// Verified and waiting for the user to save: the idle timeout governs
	// stalled transfers, not completed ones - a Ready file must not expire
	// (and delete its verified temp copy) underneath the user.
	job.idleTimer->stop();
	updateReceiveState(job, FTTransferInfo::State::Ready);
	return true;
}

// ---------------------------------------------------------------------------
// Sending

quint64 FileTransferEngine::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									  QByteArray password, const QSet< unsigned int > &recipients) {
	QFileInfo fileInfo(filePath);
	if (shutdownRequested() || !fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable()
		|| fileInfo.size() <= 0) {
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
	if (m_config.sendRateBytesPerSecond > 0) {
		// Aim for progress every quarter second, respecting the protocol's
		// minimum. At the UI's minimum 1 KiB/s, a 16 KiB chunk takes ~16 s,
		// rather than letting a configured 1 MiB chunk exceed the 60 s idle limit.
		const quint32 rateChunk = qMax< quint32 >(MinChunkSize, m_config.sendRateBytesPerSecond / 4);
		job->effectiveChunkSize = qMin(job->effectiveChunkSize, rateChunk);
	}
	job->file			= new QFile(fileInfo.absoluteFilePath());
	if (!job->file->open(QIODevice::ReadOnly)) {
		cleanupSend(*job, true);
		zeroize(password);
		return 0;
	}
	const quint32 chunkSize  = job->effectiveChunkSize;
	const quint64 chunkCount = (job->fileSize + chunkSize - 1) / chunkSize;

	// One hashing pass, cancellable between bounded chunks and Merkle nodes.
	job->merkleRoot = merkleRootFromDevice(*job->file, chunkSize, chunkCount, shutdownRequested);
	if (shutdownRequested() || job->merkleRoot.isEmpty()) {
		cleanupSend(*job, true);
		zeroize(password);
		return 0;
	}
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
		if (shutdownRequested()) {
			cleanupSend(*job, true);
			zeroize(password);
			return 0;
		}
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
		cleanupSend(*job, true);
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
	if (shutdownRequested())
		return;
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
		if (shutdownRequested())
			return;
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

	if (job.lastState != FTTransferInfo::State::Transferring)
		return;
	m_sendPaceOrder.append(job.transferId);
	if (!m_sendPaceTimer->isActive()) {
		m_sendCreditMilliBytes = 0;
		m_sendCreditLastNSecs  = 0;
		m_sendCreditRemainder  = 0;
		m_sendPaceClock.start();
		m_sendPaceTimer->start(50);
	}
}

void FileTransferEngine::refillSendCredit() {
	// Keep the clock running: restarting it for every packet would discard
	// all sub-millisecond time. Carry the division remainder as well.
	const qint64 nowNSecs     = m_sendPaceClock.nsecsElapsed();
	const qint64 elapsedNSecs = qBound< qint64 >(qint64(0), nowNSecs - m_sendCreditLastNSecs, qint64(1000000000));
	m_sendCreditLastNSecs     = nowNSecs;
	const qint64 rate         = m_config.sendRateBytesPerSecond;
	if (rate > 0) {
		// Accumulate enough credit for one whole chunk, even at low rates.
		// Late timer delivery permits at most a chunk plus one normal tick's burst.
		qint64 capacity = 0;
		for (const QByteArray &id : m_sendPaceOrder) {
			const auto job = m_sendJobs.value(id);
			if (job)
				capacity = qMax(capacity, qint64(job->effectiveChunkSize) + TagSize);
		}
		capacity += rate / 20;
		// Even the full quint32 rate times the capped one-second interval
		// fits qint64. Credit is stored in thousandths of a ciphertext byte.
		const qint64 earned    = rate * elapsedNSecs + m_sendCreditRemainder;
		m_sendCreditRemainder  = earned % 1000000;
		m_sendCreditMilliBytes = qMin(capacity * 1000, m_sendCreditMilliBytes + earned / 1000000);
		if (m_sendCreditMilliBytes == capacity * 1000)
			m_sendCreditRemainder = 0;
	}
}

void FileTransferEngine::paceSends() {
	// Yield to cancellation and incoming controls, including at unlimited rate.
	// This bounds one engine dispatch, not the downstream GUI/socket queues.
	qint64 dispatchedBytes = 0;
	int dispatchedChunks   = 0;
	bool waitingForCredit  = false;
	while (!m_sendPaceOrder.isEmpty() && dispatchedChunks < 32 && dispatchedBytes < 4 * 1024 * 1024) {
		if (shutdownRequested())
			return;
		const QByteArray id = m_sendPaceOrder.first();
		const auto job      = m_sendJobs.value(id); // Keep it alive across synchronous test callbacks.
		if (!job || job->lastState != FTTransferInfo::State::Transferring) {
			m_sendPaceOrder.removeFirst();
			continue;
		}
		const qint64 bytes =
			static_cast< qint64 >(qMin< quint64 >(job->effectiveChunkSize, job->fileSize - job->bytesDone)) + TagSize;
		refillSendCredit();
		if (m_config.sendRateBytesPerSecond > 0) {
			if (m_sendCreditMilliBytes < bytes * 1000) {
				waitingForCredit = true;
				break;
			}
		}
		// Keep this job queued during callbacks so cleaning up another job
		// cannot mistake an active sender for an empty queue and stop its timer.
		sendNextChunk(*job);
		m_sendPaceOrder.removeAll(id);
		++dispatchedChunks;
		dispatchedBytes += bytes;
		if (m_sendJobs.value(id) == job && job->lastState == FTTransferInfo::State::Transferring)
			m_sendPaceOrder.append(id);
	}
	if (m_sendPaceOrder.isEmpty()) {
		m_sendPaceTimer->stop();
		m_sendCreditMilliBytes = 0;
	} else {
		// Yield to controls without imposing a 50 ms throughput cap per batch.
		m_sendPaceTimer->start(waitingForCredit ? 50 : 0);
	}
}

void FileTransferEngine::sendNextChunk(SendJob &job) {
	if (job.lastState == FTTransferInfo::State::Aborted
		|| job.lastState == FTTransferInfo::State::Failed) {
		return;
	}
	const quint32 chunkSize = job.effectiveChunkSize;

	if (job.nextChunkIndex < job.chunkCount) {
		if (shutdownRequested())
			return;
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
		if (shutdownRequested())
			return;
		// Charge at dispatch, after potentially slow file reads/encryption.
		// Refresh between packets so a paused callback cannot spend stale credit
		// and then immediately refill a second burst on the next timer event.
		refillSendCredit();
		if (m_config.sendRateBytesPerSecond > 0)
			m_sendCreditMilliBytes -= static_cast< qint64 >(ciphertext.size()) * 1000;
		const quint64 index = job.nextChunkIndex;
		job.bytesDone += static_cast< quint64 >(chunk.size());
		++job.nextChunkIndex;
		if (m_transportChunk)
			m_transportChunk(job.transferId, index, job.chunkCount, ciphertext);
		if (job.lastState != FTTransferInfo::State::Transferring)
			return;
	}

	if (job.bytesDone - job.lastProgressBytes >= 1024 * 1024
		|| job.nextChunkIndex >= job.chunkCount) {
		job.lastProgressBytes = job.bytesDone;
		updateSendState(job, FTTransferInfo::State::Transferring);
		if (job.lastState != FTTransferInfo::State::Transferring)
			return;
	}

	if (job.nextChunkIndex >= job.chunkCount) {
		QList< QPair< unsigned int, QByteArray > > completions;
		for (const SendPeer &peer : job.peers) {
			if (peer.established && peer.session_) {
				const QByteArray frame =
					peer.session_->sealControl(FTFrame::TypeComplete, canonicalEmptyMap);
				if (!frame.isEmpty())
					completions.append({ peer.session, frame });
			}
		}
		for (const auto &completion : completions) {
			if (m_transportControl)
				m_transportControl(completion.first, completion.second);
			if (job.lastState != FTTransferInfo::State::Transferring)
				return;
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
	m_sendPaceOrder.removeAll(job.transferId);
	if (m_sendPaceOrder.isEmpty()) {
		m_sendPaceTimer->stop();
		m_sendCreditMilliBytes = 0;
	}
	job.handshakeTimer.reset();
	for (SendPeer &peer : job.peers) {
		delete peer.session_;
		peer.session_ = nullptr;
	}
	job.peers.clear();
	m_sendJobs.remove(job.transferId);
}

void FileTransferEngine::cleanupReceive(std::shared_ptr< ReceiveJob > jobPtr, bool keepReady) {
	ReceiveJob &job = *jobPtr;
	// The map's entry may be the only owning reference. The by-value
	// parameter took its own owning copy at the call, so erasing the entry
	// here cannot destroy the job (or the shared_ptr the caller passed, which
	// may literally live inside the map node) while we still work on it.
	const QByteArray transferId    = job.transferId;
	const unsigned int peerSession = job.peerSession;
	// Ready output may survive while the engine is alive. Shutdown also
	// removes unsaved plaintext; files explicitly saved elsewhere are untouched.
	const bool keepTemp = (keepReady && job.lastState == FTTransferInfo::State::Ready);

	if (!transferId.isEmpty()) {
		m_receiveJobs.remove(transferId);
	} else {
		// Pre-manifest jobs live under the synthetic per-peer key. Removing
		// that key unconditionally could delete ANOTHER job's slot once a
		// second transfer from the same peer exists.
		m_receiveJobs.remove(preManifestKey(peerSession));
	}

	if (job.spool) {
		job.spool->close();
		delete job.spool;
		job.spool = nullptr;
	}
	if (!keepTemp) {
		job.tempDirectory.reset();
	}
	zeroize(job.fileKey);
	job.idleTimer.reset();
	job.session_.reset();
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
		const std::shared_ptr< ReceiveJob > jobPtr = recvIt.value();
		updateReceiveState(*jobPtr, FTTransferInfo::State::Aborted);
		cleanupReceive(jobPtr);
	}
}

void FileTransferEngine::saveTransferAs(const QByteArray &transferId, const QString &targetPath,
										bool replaceConfirmed) {
	auto it = m_receiveJobs.find(transferId);
	if (it == m_receiveJobs.end() || it->get()->lastState != FTTransferInfo::State::Ready) {
		return;
	}
	const std::shared_ptr< ReceiveJob > jobPtr = it.value();
	ReceiveJob &job									  = *jobPtr;

	const QFileInfo targetInfo(targetPath);
	if (targetInfo.isSymLink() || !targetInfo.dir().mkpath(".")) {
		emitSaveFailed(job);
		return;
	}

	QFile source(job.tempFile);
	if (!source.open(QIODevice::ReadOnly)) {
		emitSaveFailed(job);
		return;
	}
	const auto copyReceived = [&](QIODevice &output) {
		quint64 copied = 0;
		while (!source.atEnd()) {
			if (shutdownRequested())
				return false;
			const QByteArray block = source.read(256 * 1024);
			if (block.isEmpty() || output.write(block) != block.size())
				return false;
			copied += static_cast< quint64 >(block.size());
		}
		return source.error() == QFile::NoError && copied == job.fileSize;
	};
	bool committed = false;
	bool stageRemoved = true;
	if (replaceConfirmed) {
		QSaveFile output(targetPath);
		// Never truncate the original as a fallback when staging is unavailable.
		output.setDirectWriteFallback(false);
		if (output.open(QIODevice::WriteOnly) && copyReceived(output) && m_syncSaveFile(output)) {
			source.close();
			committed = output.commit();
		} else {
			output.cancelWriting();
		}
	} else {
		// Own an unpredictable stage in the destination directory. Publishing
		// a hard link is atomic and refuses even a target created during the copy.
		// A filesystem without hard-link support fails closed, leaving Ready.
		QTemporaryFile stage(targetInfo.dir().filePath(".mumble-ft-save-XXXXXX"));
		if (stage.open() && copyReceived(stage) && m_syncSaveFile(stage)) {
			const QString stagePath = stage.fileName();
			stage.close();
			source.close();
#ifdef Q_OS_WIN
			const std::filesystem::path from(stagePath.toStdWString()), to(targetPath.toStdWString());
#else
			const std::filesystem::path from(QFile::encodeName(stagePath).constData()),
				to(QFile::encodeName(targetPath).constData());
#endif
			std::error_code error;
			if (stage.error() == QFile::NoError)
				std::filesystem::create_hard_link(from, to, error);
			else
				error = std::make_error_code(std::errc::io_error);
			committed = !error;
			if (committed)
				stageRemoved = stage.remove();
		}
	}
	source.close();
	if (!committed) {
		emitSaveFailed(job);
	} else if (!stageRemoved) {
		emitSaveFailed(
			job,
			tr("The file was written, but its temporary copy could not be removed. Received data is kept for retry."));
	} else if (!m_syncSavePublication(targetPath)) {
		// Publication has already changed the destination. Do not pretend that
		// this is a pre-commit failure or discard our retryable received copy.
		emitSaveFailed(job, tr("The file was written, but disk synchronization could not be confirmed. Received data "
							   "is kept for retry."));
	} else {
		emitSaveDone(jobPtr);
	}
}

void FileTransferEngine::providePassword(const QByteArray &transferId, QByteArray password) {
	if (shutdownRequested()) {
		zeroize(password);
		return;
	}
	auto it = m_receiveJobs.find(transferId);
	if (it == m_receiveJobs.end() || !it->get()->waitingPassword) {
		zeroize(password);
		return;
	}
	const std::shared_ptr< ReceiveJob > jobPtr = it.value();
	ReceiveJob &job									  = *jobPtr;

	{
		QByteArray pw = password;
		SecureBytes pwKey;
		if (!argon2idDerive(pwKey, pw, job.manifest.argon2Salt, *job.manifest.argon2)) {
			zeroize(password);
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}
		zeroize(password);
		if (shutdownRequested())
			return;

		const QByteArray pwWrapKey =
			passwordWrapKey(pwKey.toByteArray(), job.transferId, job.manifest.fpA, job.manifest.fpB);
		QByteArray layer1;
		if (!unwrapPasswordLayer(layer1, job.manifest.wrappedFileKey, pwWrapKey, job.transferId,
								 true, job.manifest.fpA, job.manifest.fpB, *job.manifest.argon2,
								 job.manifest.argon2Salt)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}

		const QByteArray sessionKek = job.session_->deriveSessionKek(job.transferId);
		const QByteArray wrapNonce1  = job.session_->deriveWrapNonce1(job.transferId);
		QByteArray fileKey;
		if (!unwrapFileKeySessionLayer(fileKey, layer1, sessionKek, wrapNonce1, job.transferId, true,
									   job.manifest.fpA, job.manifest.fpB)) {
			updateReceiveState(job, FTTransferInfo::State::Failed, genericDecryptionError());
			cleanupReceive(jobPtr);
			return;
		}
		job.fileKey = fileKey;
	}

	job.waitingPassword = false;
	updateReceiveState(job, FTTransferInfo::State::Transferring);
	drainEarlyChunks(jobPtr);
	// Everything that arrived while the password prompt was up was spooled
	// to disk; replay it now that the key is available.
	drainSpooledChunks(jobPtr);
	if (job.receivedCount == job.manifest.chunkCount) {
		tryCompleteReceive(jobPtr);
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
	for (PendingHandshake &pending : m_pendingHandshakes) {
		stopPendingHandshakeTimer(pending);
	}
	m_pendingHandshakes.clear();
}

void FileTransferEngine::stopPendingHandshakeTimer(PendingHandshake &pending) {
	if (pending.timeout) {
		pending.timeout->stop();
		pending.timeout->deleteLater();
		pending.timeout = nullptr;
	}
}

void FileTransferEngine::resolveFirstContact(unsigned int peerSession, bool verified) {
	auto it = m_pendingHandshakes.find(peerSession);
	if (it == m_pendingHandshakes.end()) {
		return;
	}
	PendingHandshake pending = std::move(it.value());
	m_pendingHandshakes.erase(it);
	stopPendingHandshakeTimer(pending);

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
	info.peerName        = job.peerName;
	info.peerFingerprint = job.session_ ? job.session_->peerFingerprint() : QByteArray();
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

void FileTransferEngine::emitSaveDone(std::shared_ptr< ReceiveJob > jobPtr) {
	ReceiveJob &job = *jobPtr;
	job.lastState = FTTransferInfo::State::Saved;
	FTTransferInfo info;
	info.transferId  = job.transferId;
	info.peerSession = job.peerSession;
	info.peerName        = job.peerName;
	info.peerFingerprint = job.session_ ? job.session_->peerFingerprint() : QByteArray();
	info.incoming	= true;
	info.fileName	= job.fileName;
	info.fileSize	= job.fileSize;
	info.state		 = FTTransferInfo::State::Saved;
	emitInfo(info);
	cleanupReceive(jobPtr);
}

void FileTransferEngine::emitSaveFailed(ReceiveJob &job, const QString &error) {
	FTTransferInfo info;
	info.transferId  = job.transferId;
	info.peerSession = job.peerSession;
	info.peerName        = job.peerName;
	info.peerFingerprint = job.session_ ? job.session_->peerFingerprint() : QByteArray();
	info.incoming	= true;
	info.fileName	= job.fileName;
	info.fileSize	= job.fileSize;
	info.state		 = FTTransferInfo::State::Ready;
	info.error       = error.isEmpty() ? tr("Could not save the file") : error;
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
