// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/engine/FTMessages.h"
#include "PQFileTransfer/engine/FileTransferTransport.h"

#include "Channel.h"
#include "ClientUser.h"
#include "Database.h"
#include "ServerHandler.h"
#include "Global.h"

#include <QFileInfo>
#include <QThread>
#include <climits>

#include <algorithm>
#include <utility>

FileTransferManager::FileTransferManager(QObject *parent) : QObject(parent) {
	qRegisterMetaType< PQFT::FTTransferInfo >("PQFT::FTTransferInfo");

	if (Global::get().db) {
		const QSqlDatabase connection = Global::get().db->connection();
		PQFT::FileTransferIdentity::ensureSchema(connection);
		m_identity   = std::make_unique< FileTransferIdentity >(connection);
		m_trustStore = std::make_unique< PeerTrustStore >(connection);
	}

	m_workerThread = std::make_unique< QThread >(this);
	m_workerThread->setObjectName(QStringLiteral("FileTransferWorker"));
	m_engine = std::make_unique< PQFT::FileTransferEngine >().release();
	setupEngineTransports();
	m_engine->moveToThread(m_workerThread.get());
	connect(m_workerThread.get(), &QThread::finished, m_engine, &QObject::deleteLater);
	m_workerThread->start();

	applyEngineConfig();
}

FileTransferManager::~FileTransferManager() {
	m_workerThread->requestInterruption();
	m_workerThread->quit();
	// The engine still uses this manager's identity and pin cache. Keep them
	// alive until its current operation and worker-owned cleanup have finished.
	m_workerThread->wait();
}

void FileTransferManager::setupEngineTransports() {
	// Engine pin lookups run on the worker thread against the cache
	PQFT::FileTransferEngine *engine         = m_engine;
	QMutex *cacheMutex                       = &m_pinCacheMutex;
	QHash< unsigned int, QByteArray > *cache = &m_pinCache;
	engine->setPinLookup([cacheMutex, cache](unsigned int peerSession) -> QByteArray {
		QMutexLocker lock(cacheMutex);
		return cache->value(peerSession);
	});

	engine->setPeerNameLookup([this](unsigned int peerSession) {
		QMutexLocker lock(&m_pinCacheMutex);
		return m_peerNameCache.value(peerSession);
	});

	engine->setPeerLookup([this](unsigned int session) {
		QMutexLocker lock(&m_pinCacheMutex);
		return PQFT::FileTransferEngine::PeerSnapshot{ m_peerNameCache.value(session), m_pinCache.value(session),
													   m_peerNameCache.contains(session)
														   && !m_unavailablePeers.contains(session) };
	});

	connect(
		engine, &PQFT::FileTransferEngine::firstContactRetired, this,
		[this](unsigned int session, quint64 token) {
			forwardEngineEvent([this, session, token]() {
				for (auto it = m_contactRequests.begin(); it != m_contactRequests.end();) {
					if (!it->sending && it->peerSession == session && it->workerToken == token) {
						if (m_pendingFirstContact.value(session).promptToken == it->promptToken)
							m_pendingFirstContact.remove(session);
						emit firstContactCancelled(it->promptToken);
						it = m_contactRequests.erase(it);
					} else
						++it;
				}
			});
		},
		Qt::DirectConnection);

	connect(
		engine, &PQFT::FileTransferEngine::transferUpdated, this,
		[this](const PQFT::FTTransferInfo &info) {
			forwardEngineEvent([this, info]() {
				if (!info.incoming && info.sendContext != 0
					&& (info.state == PQFT::FTTransferInfo::State::Saved
						|| info.state == PQFT::FTTransferInfo::State::PartiallyDelivered
						|| info.state == PQFT::FTTransferInfo::State::Failed
						|| info.state == PQFT::FTTransferInfo::State::Aborted)) {
					m_sendSelections.remove(info.sendContext);
					for (auto it = m_contactRequests.begin(); it != m_contactRequests.end();) {
						if (it->sending && it->transferId == info.transferId) {
							emit firstContactCancelled(it->promptToken);
							it = m_contactRequests.erase(it);
						} else
							++it;
					}
				}
				if (info.incoming && !info.transferId.isEmpty()) {
					if (info.state == PQFT::FTTransferInfo::State::Transferring
						|| info.state == PQFT::FTTransferInfo::State::WaitingPassword)
						m_activeReceived.insert(info.transferId);
					if (info.state == PQFT::FTTransferInfo::State::Ready
						|| info.state == PQFT::FTTransferInfo::State::Saved
						|| info.state == PQFT::FTTransferInfo::State::Failed
						|| info.state == PQFT::FTTransferInfo::State::Aborted) {
						m_activeReceived.remove(info.transferId);
						m_inboundRejected.remove(info.transferId);
					}
					if (info.state == PQFT::FTTransferInfo::State::Ready
						|| info.state == PQFT::FTTransferInfo::State::Saved
						|| info.state == PQFT::FTTransferInfo::State::Failed
						|| info.state == PQFT::FTTransferInfo::State::Aborted)
						m_savesInFlight.remove(info.transferId);
					const auto rejected = m_rejectedReceived.constFind(info.transferId);
					if (rejected != m_rejectedReceived.constEnd()) {
						if (info.state == PQFT::FTTransferInfo::State::Saved) {
							// A save authorized before rejection may already have completed. Report it truthfully.
							m_rejectedReceived.remove(info.transferId);
							emit transferUpdated(info);
							return;
						}
						// Worker abort is cleanup; preserve the useful trust failure on the card.
						if (info.state == PQFT::FTTransferInfo::State::Aborted
							|| info.state == PQFT::FTTransferInfo::State::Failed) {
							const auto failure = rejected.value();
							m_rejectedReceived.remove(info.transferId);
							emit transferUpdated(failure);
						}
						return;
					}
					if (!Global::get().s.bFTRequireVerification && info.error.isEmpty()
						&& (info.state == PQFT::FTTransferInfo::State::Transferring
							|| info.state == PQFT::FTTransferInfo::State::WaitingPassword
							|| info.state == PQFT::FTTransferInfo::State::Verifying
							|| info.state == PQFT::FTTransferInfo::State::Ready)
						&& !adoptFirstUsePin(info))
						return;
					if (info.state == PQFT::FTTransferInfo::State::Ready) {
						QByteArray pinned;
						const auto state = trustStateForTransfer(info, pinned);
						if (state != PQFT::TrustState::Pinned && state != PQFT::TrustState::Verified) {
							rejectReceive(info, state);
							return;
						}
						m_readyReceived.insert(info.transferId, info);
					} else if (info.state == PQFT::FTTransferInfo::State::Saved
							   || info.state == PQFT::FTTransferInfo::State::Failed
							   || info.state == PQFT::FTTransferInfo::State::Aborted) {
						m_readyReceived.remove(info.transferId);
					}
				}
				emit transferUpdated(info);
			});
		},
		Qt::DirectConnection);
	connect(
		engine, &PQFT::FileTransferEngine::firstContact, this,
		[this](unsigned int peerSession, const QByteArray &fingerprint, const QString &safety,
			   const QByteArray &transferId, bool sending, quint64 workerToken, const QString &capturedName,
			   quint64 sendContext) {
			forwardEngineEvent([this, peerSession, fingerprint, safety, transferId, sending, workerToken, capturedName,
								sendContext]() {
				auto resume = [this, peerSession, transferId, sending, workerToken](bool accepted) {
					QMetaObject::invokeMethod(
						m_engine,
						[this, peerSession, transferId, sending, workerToken, accepted]() {
							if (sending)
								m_engine->resolveSendAuthorization(peerSession, transferId, accepted);
							else
								m_engine->resolveFirstContact(peerSession, accepted, workerToken);
						},
						Qt::QueuedConnection);
				};
				ClientUser *user        = ClientUser::get(peerSession);
				const ClientUser *self  = ClientUser::get(Global::get().uiSession);
				const QByteArray digest = serverDigest();
				if (!m_trustStore || !user || !user->bFileTransferCapable || !Global::get().s.bFTEnabled
					|| digest.isEmpty() || !self || !self->cChannel || user->cChannel != self->cChannel
					|| user->qsName != capturedName) {
					resume(false);
					return;
				}
				if (sending) {
					bool current = false;
					for (const SendRecipient &selected : m_sendSelections.value(sendContext)) {
						if (selected.session == peerSession && selected.user == user && selected.name == capturedName
							&& selected.serverDigest == digest && selected.generation == m_connectionGeneration
							&& selected.channelId == self->cChannel->iId)
							current = true;
					}
					if (!current) {
						resume(false);
						return;
					}
				}
				if (!sending) {
					// A replacement M1 owns a new worker token, even with the same key.
					for (auto it = m_contactRequests.begin(); it != m_contactRequests.end();) {
						if (!it->sending && it->user == user && it->workerToken != workerToken) {
							emit firstContactCancelled(it->promptToken);
							it = m_contactRequests.erase(it);
						} else
							++it;
					}
				}
				const auto state = m_trustStore->check(digest, capturedName, fingerprint, safety);
				if (state == PQFT::TrustState::Changed || state == PQFT::TrustState::StorageError) {
					resume(false);
					if (state == PQFT::TrustState::Changed)
						emit peerBlocked(peerSession, capturedName);
					return;
				}
				if (state == PQFT::TrustState::Pinned || state == PQFT::TrustState::Verified) {
					// The live check confirmed this peer only; update its entry without scanning the channel.
					{
						QMutexLocker lock(&m_pinCacheMutex);
						m_pinCache.insert(peerSession, fingerprint);
						m_unavailablePeers.remove(peerSession);
					}
					resume(true);
					return;
				}
				if (!Global::get().s.bFTRequireVerification) {
					// Trust on first use: no prompt. Verification stays available from the
					// user menu, and a later key change is still hard-blocked.
					if (sending) {
						// M4 proved the recipient holds this key, so it can be pinned now.
						const auto pinned = m_trustStore->checkAndPin(digest, capturedName, fingerprint, safety);
						if (pinned != PQFT::TrustState::Pinned && pinned != PQFT::TrustState::Verified) {
							resume(false);
							if (pinned == PQFT::TrustState::Changed)
								emit peerBlocked(peerSession, capturedName);
							return;
						}
						QMutexLocker lock(&m_pinCacheMutex);
						m_pinCache.insert(peerSession, fingerprint);
						m_unavailablePeers.remove(peerSession);
					}
					// Receiving: the M1 key is only a claim. Let the handshake run and pin
					// once the sender's signed manifest has been verified (adoptFirstUsePin).
					resume(true);
					return;
				}
				PendingFirstContact pending{ fingerprint,
											 safety,
											 QPointer< ClientUser >(user),
											 capturedName,
											 digest,
											 ++m_nextPromptToken,
											 transferId,
											 workerToken,
											 sending,
											 self->cChannel->iId,
											 m_connectionGeneration,
											 peerSession };
				m_contactRequests.insert(pending.promptToken, pending);
				m_pendingFirstContact.insert(peerSession, pending);
				emit firstContact(peerSession, fingerprint, safety);
			});
		},
		Qt::DirectConnection);
	connect(
		engine, &PQFT::FileTransferEngine::peerBlocked, this,
		[this](unsigned int peerSession) {
			forwardEngineEvent([this, peerSession]() {
				const ClientUser *user = ClientUser::get(peerSession);
				emit peerBlocked(peerSession, user ? user->qsName : tr("Unknown user"));
			});
		},
		Qt::DirectConnection);
	connect(
		engine, &PQFT::FileTransferEngine::passwordRequired, this,
		[this](const QByteArray &transferId) {
			forwardEngineEvent([this, transferId]() { emit passwordRequired(transferId, QString()); });
		},
		Qt::DirectConnection);
}

void FileTransferManager::forwardEngineEvent(std::function< void() > event) {
	// Read only the worker-owned generation here. GUI invalidation can happen
	// while the worker is busy, before its queued cleanup has had time to run.
	const quint64 generation = m_engineGeneration;
	QMetaObject::invokeMethod(
		this,
		[this, generation, event = std::move(event)]() {
			if (generation == m_connectionGeneration) {
				event();
			}
		},
		Qt::QueuedConnection);
}

void FileTransferManager::updateConnectionTransport() {
	if (const auto handler = Global::get().sh)
		handler->setFileTransferReceiveBudget(m_inboundBytes);
	if (m_transportSetup) {
		const auto setup = m_transportSetup;
		QMetaObject::invokeMethod(m_engine, [this, setup]() { setup(*m_engine); }, Qt::QueuedConnection);
		return;
	}
	// Only the GUI thread reads Global::sh. Worker callbacks retain this exact
	// connection, so reconnects cannot race a shared_ptr read or redirect old data.
	const std::weak_ptr< ServerHandler > connection = Global::get().sh;
	const int connectionId                          = Global::get().sh ? Global::get().sh->getConnectionID() : -1;
	PQFT::FileTransferEngine *engine                = m_engine;
	QMetaObject::invokeMethod(
		engine,
		[engine, connection, connectionId]() {
			engine->setTransport(
				[connection, connectionId](unsigned int targetSession, const QByteArray &payload) {
					PQFT::Transport::queueControl(connection, targetSession, payload, connectionId);
				},
				{});
			engine->setTargetedChunkTransport(
				[connection, connectionId](const QList< unsigned int > &targets, const QByteArray &id, quint64 index,
										   quint64 total, const QByteArray &data) {
					return PQFT::Transport::queueChunk(connection, targets, id, index, total, data, connectionId);
				},
				true);
		},
		Qt::QueuedConnection);
}

QByteArray FileTransferManager::serverDigest() const {
	if (m_serverDigestProvider)
		return m_serverDigestProvider();
	auto sh = Global::get().sh;
	return sh ? sh->qbaDigest : QByteArray();
}

void FileTransferManager::handleControlMessage(const MumbleProto::FileTransferControl &msg) {
	// User and trust-change events maintain the cache. Untrusted controls
	// must never trigger a channel-wide database scan.
	const unsigned int actor = msg.has_actor() ? msg.actor() : 0;
	if (actor == 0 || !msg.has_payload()) {
		return;
	}
	const qint64 charge   = static_cast< qint64 >(msg.payload().size());
	const qint64 previous = m_inboundBytes->fetch_add(charge);
	if (charge > 64 * 1024 || previous + charge > 8 * 1024 * 1024 + 64 * 1024) {
		m_inboundBytes->fetch_sub(charge);
		return;
	}
	const QByteArray payload(msg.payload().data(), static_cast< int >(msg.payload().size()));
	quint8 frameType = 0;
	if (PQFT::FTFrame::decodeHeader(payload, frameType) && frameType == PQFT::FTFrame::TypeM1 && m_trustStore
		&& Global::get().s.bFTEnabled
		&& (!m_unavailablePeerRetryClock.isValid() || m_unavailablePeerRetryClock.elapsed() >= 1000)) {
		bool unavailable;
		{
			QMutexLocker lock(&m_pinCacheMutex);
			unavailable = m_unavailablePeers.contains(actor);
		}
		const auto *user        = ClientUser::get(actor);
		const auto *self        = ClientUser::get(Global::get().uiSession);
		const QByteArray digest = serverDigest();
		if (unavailable && user && self && self->cChannel && user->cChannel == self->cChannel
			&& user->bFileTransferCapable && !digest.isEmpty()) {
			// Recover a failed lookup with at most one database query per second,
			// regardless of channel size or the volume of untrusted controls.
			m_unavailablePeerRetryClock.restart();
			PQFT::PinnedPeer peer;
			bool succeeded    = false;
			const bool pinned = m_trustStore->lookup(peer, digest, user->qsName, &succeeded);
			if (succeeded) {
				QMutexLocker lock(&m_pinCacheMutex);
				m_peerNameCache.insert(actor, user->qsName);
				if (pinned)
					m_pinCache.insert(actor, peer.fingerprint);
				else
					m_pinCache.remove(actor);
				m_unavailablePeers.remove(actor);
			}
		}
	}
	PQFT::FileTransferEngine::PeerSnapshot snapshot;
	{
		QMutexLocker lock(&m_pinCacheMutex);
		snapshot = { m_peerNameCache.value(actor), m_pinCache.value(actor),
					 m_peerNameCache.contains(actor) && !m_unavailablePeers.contains(actor) };
	}
	QMetaObject::invokeMethod(
		m_engine,
		[this, actor, payload, snapshot, charge, generation = m_connectionGeneration]() {
			m_inboundBytes->fetch_sub(charge);
			if (generation == m_engineGeneration)
				m_engine->onControlMessage(actor, payload, snapshot);
		},
		Qt::QueuedConnection);
}

void FileTransferManager::handleDataMessage(const MumbleProto::FileData &msg) {
	if (!msg.has_transfer_id() || !msg.has_data() || msg.transfer_id().size() != 16) {
		return;
	}
	const unsigned int actor = msg.has_actor() ? msg.actor() : 0;
	if (actor == 0) {
		return;
	}
	const QByteArray transferId(msg.transfer_id().data(), static_cast< int >(msg.transfer_id().size()));
	if (msg.has_relay_accepted() || msg.has_relay_retry_after_ms()) {
		if (actor != Global::get().uiSession)
			return;
		const bool accepted  = msg.relay_accepted();
		const quint32 retry  = msg.relay_retry_after_ms();
		const quint64 index  = msg.chunk_index();
		const QString reason = QString::fromStdString(msg.relay_error()).left(256);
		QMetaObject::invokeMethod(
			m_engine,
			[this, transferId, index, accepted, retry, reason, generation = m_connectionGeneration]() {
				if (generation == m_engineGeneration)
					m_engine->relayResponse(transferId, index, accepted, retry, reason);
			},
			Qt::QueuedConnection);
		return;
	}
	if (msg.has_relay_error()) {
		const QString reason = QString::fromStdString(msg.relay_error()).left(256);
		QMetaObject::invokeMethod(
			m_engine,
			[this, actor, transferId, reason, ownSession = Global::get().uiSession,
			 generation = m_connectionGeneration]() {
				if (generation == m_engineGeneration) {
					if (actor == ownSession)
						m_engine->relayFailed(transferId, reason);
					else
						m_engine->transportFailed(actor, transferId, reason);
				}
			},
			Qt::QueuedConnection);
		return;
	}
	const qint64 charge   = static_cast< qint64 >(msg.data().size());
	const qint64 previous = m_inboundBytes->fetch_add(charge);
	if (charge > PQFT::MaxChunkSize + PQFT::TagSize || previous + charge > 8 * 1024 * 1024) {
		m_inboundBytes->fetch_sub(charge);
		if (m_activeReceived.contains(transferId) && !m_inboundRejected.contains(transferId)) {
			m_inboundRejected.insert(transferId);
			QMetaObject::invokeMethod(
				m_engine,
				[this, actor, transferId, generation = m_connectionGeneration]() {
					if (generation == m_engineGeneration)
						m_engine->transportFailed(actor, transferId,
												  tr("Incoming file queue is full; try a lower sending rate"));
				},
				Qt::QueuedConnection);
		}
		return;
	}
	const QByteArray data(msg.data().data(), static_cast< int >(msg.data().size()));
	const quint64 index = msg.chunk_index();
	const quint64 hint  = msg.has_chunk_count() ? msg.chunk_count() : 0;
	QMetaObject::invokeMethod(
		m_engine,
		[this, actor, transferId, index, hint, data, charge, generation = m_connectionGeneration]() {
			m_inboundBytes->fetch_sub(charge);
			if (generation == m_engineGeneration)
				m_engine->onDataMessage(actor, transferId, index, hint, data);
		},
		Qt::QueuedConnection);
}

FileTransferIdentity *FileTransferManager::identity() const {
	return m_identity.get();
}

PeerTrustStore *FileTransferManager::trustStore() const {
	return m_trustStore.get();
}

bool FileTransferManager::hasUsableIdentity() const {
	return m_identity && m_identity->hasIdentity() && m_identity->isUnlocked();
}

void FileTransferManager::pushIdentityToEngine() {
	if (!m_engine || !m_identity || !m_identity->isUnlocked()) {
		return;
	}
	FileTransferIdentity *id = m_identity.get();
	const QByteArray pk      = id->publicKey();
	updateConnectionTransport();
	QMetaObject::invokeMethod(
		m_engine,
		[this, pk, id]() {
			m_engine->setIdentity(pk, [id](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
				return id->sign(sig, msg, ctx);
			});
		},
		Qt::QueuedConnection);
}

std::optional< FileTransferManager::SendRecipient > FileTransferManager::sendRecipient(unsigned int session) const {
	ClientUser *user        = ClientUser::get(session);
	const ClientUser *self  = ClientUser::get(Global::get().uiSession);
	const QByteArray digest = serverDigest();
	if (!user || user == self || !user->bFileTransferCapable || !user->bFileTransferDeliveryReceipts || !self
		|| !self->cChannel || user->cChannel != self->cChannel || !m_trustStore || digest.isEmpty()
		|| !Global::get().s.bFTEnabled)
		return std::nullopt;
	PQFT::PinnedPeer peer;
	bool querySucceeded = false;
	const bool pinned   = m_trustStore->lookup(peer, digest, user->qsName, &querySucceeded);
	if (!querySucceeded || (pinned && peer.fingerprint.size() != PQFT::HashSize))
		return std::nullopt;
	SendRecipient recipient;
	recipient.session     = session;
	recipient.name        = user->qsName;
	recipient.fingerprint = pinned ? peer.fingerprint : QByteArray();
	recipient.trust =
		pinned ? (peer.verified ? PQFT::TrustState::Verified : PQFT::TrustState::Pinned) : PQFT::TrustState::NewPeer;
	recipient.user         = user;
	recipient.serverDigest = digest;
	recipient.generation   = m_connectionGeneration;
	recipient.channelId    = self->cChannel->iId;
	return recipient;
}

QList< FileTransferManager::SendRecipient >
	FileTransferManager::availableSendRecipients(bool &identityUnavailable) const {
	identityUnavailable = false;
	QList< SendRecipient > recipients;
	const ClientUser *self = ClientUser::get(Global::get().uiSession);
	if (!self || !self->cChannel)
		return recipients;
	for (const User *member : self->cChannel->qlUsers) {
		const auto *user = static_cast< const ClientUser * >(member);
		if (!user || user == self || !user->bFileTransferCapable || !user->bFileTransferDeliveryReceipts)
			continue;
		const auto recipient = sendRecipient(user->uiSession);
		if (recipient)
			recipients.append(*recipient);
		else
			identityUnavailable = true;
	}
	return recipients;
}

bool FileTransferManager::hasPendingSendTo(unsigned int session) const {
	for (const auto &active : m_sendSelections)
		for (const SendRecipient &existing : active)
			if (existing.session == session)
				return true;
	return false;
}

bool FileTransferManager::sendRecipientStillCurrent(const SendRecipient &recipient) const {
	const auto current = sendRecipient(recipient.session);
	return current && current->user == recipient.user && current->name == recipient.name
		   && current->fingerprint == recipient.fingerprint && current->trust == recipient.trust
		   && current->serverDigest == recipient.serverDigest && current->generation == recipient.generation
		   && current->channelId == recipient.channelId;
}

bool FileTransferManager::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									const QByteArray &password, const QList< SendRecipient > &recipients) {
	m_lastSendError.clear();
	if (!hasUsableIdentity() || m_engine == nullptr || !Global::get().s.bFTEnabled || !m_relaySupported) {
		return false;
	}
	QHash< unsigned int, QByteArray > sessions;
	QHash< unsigned int, QString > names;
	if (recipients.isEmpty())
		return false;
	for (const SendRecipient &recipient : recipients) {
		if (!sendRecipientStillCurrent(recipient))
			return false;
		if (sessions.contains(recipient.session))
			return false;
		sessions.insert(recipient.session, recipient.fingerprint);
		names.insert(recipient.session, recipient.name);
	}
	if (recipients.size() > 64)
		return false;
	for (const auto &active : m_sendSelections)
		for (const SendRecipient &existing : active)
			for (const SendRecipient &recipient : recipients)
				if (existing.session == recipient.session) {
					m_lastSendError =
						tr("A transfer to %1 is already pending. Wait for it to finish or cancel it before "
						   "sending another file.")
							.arg(recipient.name);
					return false; // The peer has one pre-manifest handshake slot.
				}
	const qint64 fileSize = QFileInfo(filePath).size();
	if (fileSize <= 0)
		return false;
	const quint64 size      = static_cast< quint64 >(fileSize);
	const quint64 chunkSize = PQFT::FileTransferEngine::sendChunkSize(engineConfig(), recipients.size());
	const quint64 chunks    = size / chunkSize + (size % chunkSize != 0 ? 1 : 0);
	if (m_relayMaxSize > 0 && (size > m_relayMaxSize || chunks > (m_relayMaxSize - size) / PQFT::TagSize)) {
		m_lastSendError = tr("The file exceeds the server's file transfer limit.");
		return false;
	}
	const quint64 sendContext = ++m_nextSendContext;
	m_sendSelections.insert(sendContext, recipients);
	// Queue identity and connection updates before starting work on the engine.
	pushIdentityToEngine();

	refreshPinCache();

	const QString path = filePath;
	const QString mime = mimeType;
	const bool pwMode  = passwordMode;
	QByteArray pw      = password;
	QMetaObject::invokeMethod(
		m_engine,
		[this, path, mime, pwMode, pw, sessions, names, sendContext]() {
			if (!m_engine->startSend(path, mime, pwMode, pw, sessions, names, sendContext))
				forwardEngineEvent([this, sendContext]() {
					m_sendSelections.remove(sendContext);
					emit sendSlotsChanged();
				});
		},
		Qt::QueuedConnection);
	return true;
}

void FileTransferManager::abortTransfer(const QByteArray &transferId) {
	QMetaObject::invokeMethod(
		m_engine, [this, transferId]() { m_engine->abortTransfer(transferId); }, Qt::QueuedConnection);
}

void FileTransferManager::rejectReceive(const PQFT::FTTransferInfo &info, PQFT::TrustState state) {
	auto failed  = info;
	failed.state = PQFT::FTTransferInfo::State::Failed;
	failed.error = state == PQFT::TrustState::Changed
					   ? tr("The sender's saved identity changed. Verify the sender and try again.")
					   : tr("Cannot confirm the sender's saved identity. Verify the sender and try again.");
	m_readyReceived.remove(info.transferId);
	m_rejectedReceived.insert(info.transferId, failed);
	abortTransfer(info.transferId);
	emit transferUpdated(failed);
	if (state == PQFT::TrustState::Changed)
		emit peerBlocked(info.peerSession, info.peerName);
}

void FileTransferManager::saveTransferAs(const QByteArray &transferId, const QString &targetPath,
										 bool replaceConfirmed) {
	// A second request must not overwrite the outcome of the already authorized save.
	if (m_savesInFlight.contains(transferId))
		return;
	const auto ready = m_readyReceived.constFind(transferId);
	if (ready == m_readyReceived.constEnd())
		return;
	const auto info = ready.value();
	QByteArray pinned;
	const auto state = trustStateForTransfer(info, pinned);
	if (state != PQFT::TrustState::Pinned && state != PQFT::TrustState::Verified) {
		rejectReceive(info, state);
		return;
	}
	m_savesInFlight.insert(transferId);
	QMetaObject::invokeMethod(
		m_engine,
		[this, transferId, targetPath, replaceConfirmed]() {
			m_engine->saveTransferAs(transferId, targetPath, replaceConfirmed);
		},
		Qt::QueuedConnection);
}

void FileTransferManager::providePassword(const QByteArray &transferId, const QByteArray &password) {
	QByteArray pw = password;
	QMetaObject::invokeMethod(
		m_engine, [this, transferId, pw]() { m_engine->providePassword(transferId, pw); }, Qt::QueuedConnection);
}

bool FileTransferManager::resolveFirstContact(unsigned int peerSession, const QByteArray &shownFingerprint,
											  quint64 promptToken, bool verified) {
	const auto request = m_contactRequests.constFind(promptToken);
	if (request == m_contactRequests.constEnd() || request->fingerprint != shownFingerprint)
		return false;
	const PendingFirstContact pending = request.value();
	const auto latest                 = m_pendingFirstContact.value(peerSession);
	// pinPeer validates the exact captured request, independent of another transfer's prompt.
	m_pendingFirstContact.insert(peerSession, pending);
	const bool accepted = verified && pinPeer(peerSession, shownFingerprint, true);
	m_contactRequests.remove(promptToken);
	if (latest.promptToken != promptToken)
		m_pendingFirstContact.insert(peerSession, latest);
	else
		m_pendingFirstContact.remove(peerSession);
	QMetaObject::invokeMethod(
		m_engine,
		[this, peerSession, pending, accepted]() {
			if (pending.sending)
				m_engine->resolveSendAuthorization(peerSession, pending.transferId, accepted);
			else
				m_engine->resolveFirstContact(peerSession, accepted, pending.workerToken);
		},
		Qt::QueuedConnection);
	return accepted;
}

quint64 FileTransferManager::firstContactToken(unsigned int peerSession) const {
	return m_pendingFirstContact.value(peerSession).promptToken;
}

bool FileTransferManager::contactRequestCurrent(unsigned int session, const QByteArray &fingerprint,
												quint64 token) const {
	const auto it = m_contactRequests.constFind(token);
	return token != 0 && it != m_contactRequests.constEnd() && it->peerSession == session
		   && it->fingerprint == fingerprint && it->generation == m_connectionGeneration;
}

QString FileTransferManager::contactRequestName(quint64 token) const {
	return m_contactRequests.value(token).username;
}
bool FileTransferManager::contactRequestSending(quint64 token) const {
	return m_contactRequests.value(token).sending;
}

bool FileTransferManager::resetPeerTrust(const SendRecipient &shown) {
	if (shown.fingerprint.size() != PQFT::HashSize || !sendRecipientStillCurrent(shown)
		|| !m_trustStore->removePin(shown.serverDigest, shown.name, shown.fingerprint))
		return false;
	// An explicit reset never adopts the observed replacement key. The next handshake
	// starts over as a first contact (trust on first use, or the safety-number
	// dialog when verification is required).
	refreshPinCache();
	for (auto it = m_contactRequests.begin(); it != m_contactRequests.end();) {
		if (it->peerSession == shown.session) {
			emit firstContactCancelled(it->promptToken);
			it = m_contactRequests.erase(it);
		} else
			++it;
	}
	m_pendingFirstContact.remove(shown.session);
	QMetaObject::invokeMethod(
		m_engine, [this, session = shown.session]() { m_engine->abortPeer(session); }, Qt::QueuedConnection);
	return true;
}

bool FileTransferManager::markPeerVerified(const SendRecipient &shown) {
	if (!m_trustStore || shown.fingerprint.size() != PQFT::HashSize || !sendRecipientStillCurrent(shown)
		|| !m_trustStore->markVerified(shown.serverDigest, shown.name, shown.fingerprint))
		return false;
	refreshPinCache();
	return true;
}

bool FileTransferManager::adoptFirstUsePin(const PQFT::FTTransferInfo &info) {
	QByteArray pinned;
	if (trustStateForTransfer(info, pinned) != PQFT::TrustState::NewPeer)
		return true; // An existing pin is checked again before the file can be saved.
	const QByteArray digest = serverDigest();
	if (!m_trustStore || !m_identity || !m_identity->isUnlocked() || digest.isEmpty() || info.peerName.isEmpty()
		|| info.peerFingerprint.size() != PQFT::HashSize)
		return true; // Without a pin the Ready check rejects the file.
	// The manifest signature proved the sender holds this key: pin it unverified.
	const auto state = m_trustStore->checkAndPin(digest, info.peerName, info.peerFingerprint,
												 PQFT::safetyNumber(m_identity->fingerprint(), info.peerFingerprint));
	if (state == PQFT::TrustState::Pinned || state == PQFT::TrustState::Verified) {
		const ClientUser *user = ClientUser::get(info.peerSession);
		if (user && user->qsName == info.peerName) {
			QMutexLocker lock(&m_pinCacheMutex);
			m_pinCache.insert(info.peerSession, info.peerFingerprint);
			m_unavailablePeers.remove(info.peerSession);
		}
		return true;
	}
	rejectReceive(info, state);
	return false;
}

bool FileTransferManager::pinPeer(unsigned int peerSession, const QByteArray &shownFingerprint, bool verified) {
	const auto it = m_pendingFirstContact.constFind(peerSession);
	if (!m_trustStore || it == m_pendingFirstContact.constEnd() || shownFingerprint.size() != PQFT::HashSize)
		return false;
	const PendingFirstContact pending = it.value();
	ClientUser *user                  = ClientUser::get(peerSession);
	const ClientUser *self            = ClientUser::get(Global::get().uiSession);
	const QByteArray digest           = serverDigest();
	if (!user || !pending.user || user != pending.user.data() || user->qsName != pending.username || digest.isEmpty()
		|| digest != pending.serverDigest || shownFingerprint != pending.fingerprint || !self || !self->cChannel
		|| user->cChannel != self->cChannel || (pending.channelId != 0 && pending.channelId != self->cChannel->iId)
		|| pending.generation != m_connectionGeneration || !user->bFileTransferCapable || !Global::get().s.bFTEnabled)
		return false;
	const auto state = m_trustStore->checkAndPin(digest, pending.username, shownFingerprint, pending.safetyNumber);
	if (state != PQFT::TrustState::Pinned && state != PQFT::TrustState::Verified)
		return false;
	if (verified && !m_trustStore->markVerified(digest, pending.username, shownFingerprint))
		return false;
	PQFT::PinnedPeer stored;
	if (!m_trustStore->lookup(stored, digest, pending.username) || stored.fingerprint != shownFingerprint
		|| (verified && !stored.verified))
		return false;
	refreshPinCache();
	return true;
}

PQFT::TrustState FileTransferManager::trustStateFor(unsigned int peerSession, QByteArray &pinnedFingerprint) {
	pinnedFingerprint.clear();
	const ClientUser *user = ClientUser::get(peerSession);
	if (!user || !m_trustStore || serverDigest().isEmpty()) {
		return PQFT::TrustState::NewPeer;
	}
	PQFT::PinnedPeer peer;
	bool querySucceeded = false;
	if (!m_trustStore->lookup(peer, serverDigest(), user->qsName, &querySucceeded)) {
		return querySucceeded ? PQFT::TrustState::NewPeer : PQFT::TrustState::StorageError;
	}
	pinnedFingerprint = peer.fingerprint;
	return peer.verified ? PQFT::TrustState::Verified : PQFT::TrustState::Pinned;
}

PQFT::TrustState FileTransferManager::trustStateForTransfer(const PQFT::FTTransferInfo &info,
															QByteArray &pinnedFingerprint) {
	pinnedFingerprint.clear();
	const QByteArray digest = serverDigest();
	if (!info.incoming || info.peerName.isEmpty() || info.peerFingerprint.size() != PQFT::HashSize || !m_trustStore
		|| digest.isEmpty())
		return PQFT::TrustState::NewPeer;
	PQFT::PinnedPeer peer;
	bool querySucceeded = false;
	if (!m_trustStore->lookup(peer, digest, info.peerName, &querySucceeded))
		return querySucceeded ? PQFT::TrustState::NewPeer : PQFT::TrustState::StorageError;
	pinnedFingerprint = peer.fingerprint;
	if (peer.fingerprint != info.peerFingerprint)
		return PQFT::TrustState::Changed;
	return peer.verified ? PQFT::TrustState::Verified : PQFT::TrustState::Pinned;
}

QString FileTransferManager::safetyNumberFor(unsigned int peerSession) {
	const ClientUser *user = ClientUser::get(peerSession);
	if (!user || !m_trustStore || !m_identity || !m_identity->isUnlocked()) {
		return QString();
	}
	PQFT::PinnedPeer peer;
	if (!m_trustStore->lookup(peer, serverDigest(), user->qsName)) {
		return QString();
	}
	return PQFT::safetyNumber(m_identity->fingerprint(), peer.fingerprint);
}

void FileTransferManager::disconnectCleanup() {
	const quint64 generation = ++m_connectionGeneration;
	m_relaySupported         = false;
	m_relayRate = m_relayAggregateRate = m_relayMaxSize = 0;
	if (m_engine) {
		QMetaObject::invokeMethod(
			m_engine,
			[this, generation]() {
				// Aborted state updates belong to this cleanup, while prompts
				// queued before invalidation retain their obsolete generation.
				m_engineGeneration = generation;
				m_engine->abortAll();
				m_engine->setTransport({}, {});
				m_engine->setTargetedChunkTransport({});
			},
			Qt::QueuedConnection);
	}
	for (const auto &pending : m_contactRequests)
		emit firstContactCancelled(pending.promptToken);
	m_pendingFirstContact.clear();
	m_contactRequests.clear();
	m_sendSelections.clear();
	m_readyReceived.clear();
	m_rejectedReceived.clear();
	m_savesInFlight.clear();
	m_activeReceived.clear();
	m_inboundRejected.clear();
	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache.clear();
	m_peerNameCache.clear();
}

void FileTransferManager::updateVerifiedNames(const QByteArray &digest) {
	QSet< QString > verified;
	if (m_trustStore && !digest.isEmpty()) {
		for (const PQFT::PinnedPeer &peer : m_trustStore->list())
			if (peer.verified && peer.serverDigest == digest)
				verified.insert(peer.username);
	}
	if (verified != m_verifiedNames) {
		m_verifiedNames = std::move(verified);
		emit trustChanged();
	}
}

bool FileTransferManager::isVerifiedPeer(const ClientUser *user) const {
	return user && user->bFileTransferCapable && m_verifiedNames.contains(user->qsName);
}

void FileTransferManager::refreshPinCache() {
	if (!m_trustStore || !m_identity) {
		updateVerifiedNames(QByteArray());
		QMutexLocker lock(&m_pinCacheMutex);
		m_pinCache.clear();
		m_peerNameCache.clear();
		return;
	}
	// Re-apply limits whenever the world changes; cheap and idempotent
	applyEngineConfig();
	const QByteArray digest = serverDigest();
	updateVerifiedNames(digest);
	if (digest.isEmpty()) {
		QMutexLocker lock(&m_pinCacheMutex);
		m_pinCache.clear();
		m_peerNameCache.clear();
		return;
	}

	for (auto it = m_contactRequests.begin(); it != m_contactRequests.end();) {
		const PendingFirstContact pending = it.value();
		const ClientUser *user            = pending.user.data();
		const ClientUser *self            = ClientUser::get(Global::get().uiSession);
		if (!user || ClientUser::get(user->uiSession) != user || user->qsName != pending.username || !self
			|| !self->cChannel || self->cChannel != user->cChannel || self->cChannel->iId != pending.channelId
			|| !user->bFileTransferCapable || !Global::get().s.bFTEnabled || pending.serverDigest != digest) {
			if (user && m_pendingFirstContact.value(user->uiSession).promptToken == pending.promptToken)
				m_pendingFirstContact.remove(user->uiSession);
			const unsigned int session = pending.peerSession;
			QMetaObject::invokeMethod(
				m_engine,
				[this, pending, session]() {
					if (pending.sending)
						m_engine->relayFailed(pending.transferId, tr("Recipient changed while authorizing identity"));
					else
						m_engine->resolveFirstContact(session, false, pending.workerToken);
				},
				Qt::QueuedConnection);
			emit firstContactCancelled(it->promptToken);
			it = m_contactRequests.erase(it);
		} else {
			++it;
		}
	}
	QHash< unsigned int, QByteArray > fresh;
	QHash< unsigned int, QString > names;
	QSet< unsigned int > unavailable;
	// Only channel peers with the file-transfer capability can ever talk to
	// us; cache every user we have a pin for.
	const ClientUser *self = ClientUser::get(Global::get().uiSession);
	if (self && self->cChannel) {
		for (const User *u : self->cChannel->qlUsers) {
			const auto *user = static_cast< const ClientUser * >(u);
			if (!user || !user->bFileTransferCapable || user->uiSession == Global::get().uiSession)
				continue;
			names.insert(user->uiSession, user->qsName);
			PQFT::PinnedPeer peer;
			bool querySucceeded = false;
			if (m_trustStore->lookup(peer, digest, user->qsName, &querySucceeded)) {
				fresh.insert(user->uiSession, peer.fingerprint);
			}
			if (!querySucceeded)
				unavailable.insert(user->uiSession);
		}
	}

	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache = std::move(fresh);
	m_peerNameCache = std::move(names);
	m_unavailablePeers = std::move(unavailable);
}

PQFT::FileTransferEngine::Config FileTransferManager::engineConfig() const {
	const Settings &s = Global::get().s;
	PQFT::FileTransferEngine::Config config;
	config.authorizeSendPeers      = true;
	config.aggregateSendRate       = m_relayAggregateRate * 9 / 10;
	config.chunkSize               = static_cast< quint32 >(qBound(16, s.iFTChunkKB, 1024) * 1024);
	config.sendRateBytesPerSecond  = static_cast< quint32 >(qBound(0, s.iFTSendPaceKiB, 1 << 20)) * 1024ull;
	if (m_relayRate > 0) {
		const quint32 safeRate = static_cast< quint32 >(qMin< quint64 >(m_relayRate * 9 / 10, UINT_MAX));
		config.sendRateBytesPerSecond =
			config.sendRateBytesPerSecond == 0 ? safeRate : qMin(config.sendRateBytesPerSecond, safeRate);
	}
	config.maxReceiveSize          = static_cast< quint64 >(qMax(1, s.iFTMaxReceiveMiB)) * 1024ull * 1024ull;
	config.handshakeTimeoutMSecs   = 10000;
	config.receiveIdleTimeoutMSecs = 60000;
	return config;
}

void FileTransferManager::applyEngineConfig() {
	if (!m_engine)
		return;
	const Settings &s = Global::get().s;
	const auto config = engineConfig();
	QMetaObject::invokeMethod(m_engine, [this, config]() { m_engine->setConfig(config); }, Qt::QueuedConnection);

	// Invalidate only on the enabled-to-disabled transition.
	if (m_featureEnabled && !s.bFTEnabled) {
		disconnectCleanup();
	}
	m_featureEnabled = s.bFTEnabled;
}

void FileTransferManager::configureRelay(const MumbleProto::ServerConfig &config) {
	if (config.has_file_transfer_relay_version())
		m_relaySupported = config.file_transfer_relay_version() >= 1;
	if (config.has_file_transfer_rate())
		m_relayRate = config.file_transfer_rate();
	if (config.has_file_transfer_aggregate_rate())
		m_relayAggregateRate = config.file_transfer_aggregate_rate();
	if (config.has_max_file_transfer_size())
		m_relayMaxSize = config.max_file_transfer_size();
	applyEngineConfig();
}

QString FileTransferManager::sendAvailabilityError() const {
	if (!m_lastSendError.isEmpty())
		return m_lastSendError;
	if (!m_relaySupported)
		return tr("This server does not support private file delivery. Ask its administrator to update the server.");
	return tr("The file exceeds the server limit, or a selected recipient changed. Check the file size and choose the "
			  "recipients again.");
}
