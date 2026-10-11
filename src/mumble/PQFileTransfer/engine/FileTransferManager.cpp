// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/engine/FileTransferTransport.h"

#include "Channel.h"
#include "ClientUser.h"
#include "Database.h"
#include "ServerHandler.h"
#include "Global.h"

#include <QThread>

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

	connect(
		engine, &PQFT::FileTransferEngine::transferUpdated, this,
		[this](const PQFT::FTTransferInfo &info) {
			forwardEngineEvent([this, info]() {
				if (info.incoming && !info.transferId.isEmpty()) {
					const auto rejected = m_rejectedReceived.constFind(info.transferId);
					if (rejected != m_rejectedReceived.constEnd()) {
						// Worker abort is cleanup; preserve the useful trust failure on the card.
						if (info.state == PQFT::FTTransferInfo::State::Aborted) {
							const auto failure = rejected.value();
							m_rejectedReceived.remove(info.transferId);
							emit transferUpdated(failure);
						}
						return;
					}
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
		[this](unsigned int peerSession, const QByteArray &peerFingerprint, const QString &safetyNumberStr,
			   const QByteArray &pendingTransferId, bool pinOnObservation) {
			forwardEngineEvent([this, peerSession, peerFingerprint, safetyNumberStr, pinOnObservation,
								pendingTransferId]() {
				ClientUser *user        = ClientUser::get(peerSession);
				const QByteArray digest = serverDigest();
				auto resume             = [this, peerSession](bool accepted) {
					QMetaObject::invokeMethod(
						m_engine,
						[this, peerSession, accepted]() { m_engine->resolveFirstContact(peerSession, accepted); },
						Qt::QueuedConnection);
				};
				if (m_trustStore) {
					const ClientUser *self = ClientUser::get(Global::get().uiSession);
					if (!user || !user->bFileTransferCapable || !Global::get().s.bFTEnabled || digest.isEmpty() || !self
						|| !self->cChannel || user->cChannel != self->cChannel) {
						resume(false);
						return;
					}
					const auto state = m_trustStore->check(digest, user->qsName, peerFingerprint, safetyNumberStr);
					if (state == PQFT::TrustState::Changed) {
						if (!pendingTransferId.isEmpty())
							abortTransfer(pendingTransferId);
						m_pendingFirstContact.remove(peerSession);
						resume(false);
						refreshPinCache();
						emit peerBlocked(peerSession, user->qsName);
						return;
					}
					if (!pinOnObservation && state != PQFT::TrustState::NewPeer) {
						// A join may race the worker cache refresh. A matching stored key is already pinned;
						// resume with the fresh cache, retaining its existing verification state.
						refreshPinCache();
						resume(true);
						return;
					}
					if (pinOnObservation) {
						// Sender-side M4 has authenticated the key; receiver-side M1 remains memory-only.
						if (m_trustStore->checkAndPin(digest, user->qsName, peerFingerprint, safetyNumberStr)
							== PQFT::TrustState::Changed) {
							if (!pendingTransferId.isEmpty())
								abortTransfer(pendingTransferId);
							resume(false);
							emit peerBlocked(peerSession, user->qsName);
							return;
						}
						refreshPinCache();
					}
				}
				m_pendingFirstContact.insert(
					peerSession, PendingFirstContact{ peerFingerprint, safetyNumberStr, QPointer< ClientUser >(user),
													  user ? user->qsName : QString(), digest, ++m_nextPromptToken });
				emit firstContact(peerSession, peerFingerprint, safetyNumberStr);
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
	// Only the GUI thread reads Global::sh. Worker callbacks retain this exact
	// connection, so reconnects cannot race a shared_ptr read or redirect old data.
	const std::weak_ptr< ServerHandler > connection = Global::get().sh;
	PQFT::FileTransferEngine *engine                = m_engine;
	QMetaObject::invokeMethod(
		engine,
		[engine, connection]() {
			engine->setTransport(
				[connection](unsigned int targetSession, const QByteArray &payload) {
					PQFT::Transport::queueControl(connection, targetSession, payload);
				},
				[connection](const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) {
					PQFT::Transport::queueChunk(connection, transferId, index, total, data);
				});
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
	const unsigned int actor = msg.has_actor() ? msg.actor() : 0;
	if (actor == 0 || !msg.has_payload()) {
		return;
	}
	const QByteArray payload(msg.payload().data(), static_cast< int >(msg.payload().size()));
	QMetaObject::invokeMethod(
		m_engine, [this, actor, payload]() { m_engine->onControlMessage(actor, payload); }, Qt::QueuedConnection);
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
	const QByteArray data(msg.data().data(), static_cast< int >(msg.data().size()));
	const quint64 index = msg.chunk_index();
	const quint64 hint  = msg.has_chunk_count() ? msg.chunk_count() : 0;
	QMetaObject::invokeMethod(
		m_engine,
		[this, actor, transferId, index, hint, data]() {
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
	if (!user || user == self || !user->bFileTransferCapable || !self || !self->cChannel
		|| user->cChannel != self->cChannel || !m_trustStore || digest.isEmpty() || !Global::get().s.bFTEnabled)
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

bool FileTransferManager::sendRecipientStillCurrent(const SendRecipient &recipient) const {
	const auto current = sendRecipient(recipient.session);
	return current && current->user == recipient.user && current->name == recipient.name
		   && current->fingerprint == recipient.fingerprint && current->trust == recipient.trust
		   && current->serverDigest == recipient.serverDigest && current->generation == recipient.generation
		   && current->channelId == recipient.channelId;
}

bool FileTransferManager::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									const QByteArray &password, const QList< SendRecipient > &recipients) {
	if (!hasUsableIdentity() || m_engine == nullptr || !Global::get().s.bFTEnabled) {
		return false;
	}
	QHash< unsigned int, QByteArray > sessions;
	if (recipients.isEmpty())
		return false;
	for (const SendRecipient &recipient : recipients) {
		if (!sendRecipientStillCurrent(recipient))
			return false;
		sessions.insert(recipient.session, recipient.fingerprint);
	}
	// Queue identity and connection updates before starting work on the engine.
	pushIdentityToEngine();

	refreshPinCache();

	const QString path = filePath;
	const QString mime = mimeType;
	const bool pwMode  = passwordMode;
	QByteArray pw      = password;
	QMetaObject::invokeMethod(
		m_engine, [this, path, mime, pwMode, pw, sessions]() { m_engine->startSend(path, mime, pwMode, pw, sessions); },
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
	const auto pending = m_pendingFirstContact.constFind(peerSession);
	// An older dialog cannot accept or decline a newer request, even when the key is unchanged.
	if (pending == m_pendingFirstContact.constEnd() || pending->fingerprint != shownFingerprint
		|| pending->promptToken != promptToken)
		return false;
	const bool accepted = verified && pinPeer(peerSession, shownFingerprint, true);
	m_pendingFirstContact.remove(peerSession);
	QMetaObject::invokeMethod(
		m_engine, [this, peerSession, accepted]() { m_engine->resolveFirstContact(peerSession, accepted); },
		Qt::QueuedConnection);
	return accepted;
}

quint64 FileTransferManager::firstContactToken(unsigned int peerSession) const {
	return m_pendingFirstContact.value(peerSession).promptToken;
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
		|| user->cChannel != self->cChannel || !user->bFileTransferCapable || !Global::get().s.bFTEnabled)
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
	if (!m_trustStore->lookup(peer, serverDigest(), user->qsName)) {
		return PQFT::TrustState::NewPeer;
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
	if (!m_trustStore->lookup(peer, digest, info.peerName))
		return PQFT::TrustState::NewPeer;
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
	if (m_engine) {
		QMetaObject::invokeMethod(
			m_engine,
			[this, generation]() {
				// Aborted state updates belong to this cleanup, while prompts
				// queued before invalidation retain their obsolete generation.
				m_engineGeneration = generation;
				m_engine->abortAll();
				m_engine->setTransport({}, {});
			},
			Qt::QueuedConnection);
	}
	m_pendingFirstContact.clear();
	m_readyReceived.clear();
	m_rejectedReceived.clear();
	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache.clear();
	m_peerNameCache.clear();
}

void FileTransferManager::refreshPinCache() {
	if (!m_trustStore || !m_identity) {
		QMutexLocker lock(&m_pinCacheMutex);
		m_pinCache.clear();
		m_peerNameCache.clear();
		return;
	}
	// Re-apply limits whenever the world changes; cheap and idempotent
	applyEngineConfig();
	const QByteArray digest = serverDigest();
	if (digest.isEmpty()) {
		QMutexLocker lock(&m_pinCacheMutex);
		m_pinCache.clear();
		m_peerNameCache.clear();
		return;
	}

	QHash< unsigned int, QByteArray > fresh;
	QHash< unsigned int, QString > names;
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
			if (m_trustStore->lookup(peer, digest, user->qsName)) {
				fresh.insert(user->uiSession, peer.fingerprint);
			}
		}
	}

	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache = std::move(fresh);
	m_peerNameCache = std::move(names);
}

void FileTransferManager::applyEngineConfig() {
	if (!m_engine) {
		return;
	}
	const Settings &s = Global::get().s;
	PQFT::FileTransferEngine::Config config;
	config.chunkSize               = static_cast< quint32 >(qBound(16, s.iFTChunkKB, 1024) * 1024);
	config.sendRateBytesPerSecond  = static_cast< quint32 >(qBound(0, s.iFTSendPaceKiB, 1 << 20)) * 1024ull;
	config.maxReceiveSize          = static_cast< quint64 >(qMax(1, s.iFTMaxReceiveMiB)) * 1024ull * 1024ull;
	config.handshakeTimeoutMSecs   = 10000;
	config.receiveIdleTimeoutMSecs = 60000;
	QMetaObject::invokeMethod(m_engine, [this, config]() { m_engine->setConfig(config); }, Qt::QueuedConnection);

	// A disabled feature stops everything in flight
	if (!s.bFTEnabled) {
		disconnectCleanup();
	}
}
