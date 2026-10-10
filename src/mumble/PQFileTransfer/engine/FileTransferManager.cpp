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
	m_workerThread->quit();
	m_workerThread->wait(3000);
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

	connect(
		engine, &PQFT::FileTransferEngine::transferUpdated, this,
		[this](const PQFT::FTTransferInfo &info) {
			forwardEngineEvent([this, info]() { emit transferUpdated(info); });
		},
		Qt::DirectConnection);
	connect(
		engine, &PQFT::FileTransferEngine::firstContact, this,
		[this](unsigned int peerSession, const QByteArray &peerFingerprint, const QString &safetyNumberStr,
			   const QByteArray &, bool pinOnObservation) {
			forwardEngineEvent([this, peerSession, peerFingerprint, safetyNumberStr, pinOnObservation]() {
				if (pinOnObservation) {
					// Sender side: the handshake (M4, signatures + MACs) has
					// already authenticated the presented key — pin on first
					// observation (the documented TOFU deviation). The user
					// can still verify later from the dialog.
					if (const ClientUser *user = ClientUser::get(peerSession)) {
						if (m_trustStore && !serverDigest().isEmpty()) {
							PQFT::PinnedPeer existing;
							if (!m_trustStore->lookup(existing, serverDigest(), user->qsName)) {
								m_trustStore->checkAndPin(serverDigest(), user->qsName, peerFingerprint,
														  safetyNumberStr);
								refreshPinCache();
							}
						}
					}
				} else {
					// Receiver side: a plain M1 is an unauthenticated claim.
					// Keep it in memory only — the pin is written when (and
					// only when) the user accepts the safety-number dialog.
					m_pendingFirstContact.insert(peerSession, qMakePair(peerFingerprint, safetyNumberStr));
				}
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

bool FileTransferManager::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									const QByteArray &password, const QList< unsigned int > &recipients) {
	if (!hasUsableIdentity() || m_engine == nullptr || !Global::get().s.bFTEnabled) {
		return false;
	}
	QSet< unsigned int > sessions;
	for (unsigned int session : recipients) {
		sessions.insert(session);
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

void FileTransferManager::saveTransferAs(const QByteArray &transferId, const QString &targetPath) {
	QMetaObject::invokeMethod(
		m_engine, [this, transferId, targetPath]() { m_engine->saveTransferAs(transferId, targetPath); },
		Qt::QueuedConnection);
}

void FileTransferManager::providePassword(const QByteArray &transferId, const QByteArray &password) {
	QByteArray pw = password;
	QMetaObject::invokeMethod(
		m_engine, [this, transferId, pw]() { m_engine->providePassword(transferId, pw); }, Qt::QueuedConnection);
}

void FileTransferManager::resolveFirstContact(unsigned int peerSession, bool verified) {
	// GUI thread: the dialog finished. On acceptance make sure the pin the
	// engine's retry will look up actually exists (the dialog flow calls
	// pinPeer first; fall back to the in-memory presented fingerprint); on
	// decline nothing was ever persisted.
	if (verified) {
		QMutexLocker lock(&m_pinCacheMutex);
		const bool alreadyPinned = m_pinCache.contains(peerSession);
		lock.unlock();
		if (!alreadyPinned && !pinPeer(peerSession, true)) {
			verified = false; // nothing to verify against — treat as declined
		}
	}
	m_pendingFirstContact.remove(peerSession);
	const bool v = verified;
	QMetaObject::invokeMethod(
		m_engine, [this, peerSession, v]() { m_engine->resolveFirstContact(peerSession, v); }, Qt::QueuedConnection);
}

bool FileTransferManager::pinPeer(unsigned int peerSession, bool verified) {
	const ClientUser *user = ClientUser::get(peerSession);
	if (!user || !m_trustStore) {
		return false;
	}
	// Which fingerprint? For a receiver-side first contact the pin cache and
	// trust store are intentionally empty (nothing is persisted before the
	// dialog) — the manager kept the presented key in memory instead. The
	// cache covers everyone else (pinned peers, sender-side TOFU).
	QByteArray fp;
	{
		QMutexLocker lock(&m_pinCacheMutex);
		fp = m_pinCache.value(peerSession);
	}
	if (fp.isEmpty()) {
		PQFT::PinnedPeer peer;
		if (m_trustStore->lookup(peer, serverDigest(), user->qsName)) {
			fp = peer.fingerprint;
		}
	}
	if (fp.isEmpty()) {
		fp = m_pendingFirstContact.value(peerSession).first;
	}
	if (fp.isEmpty()) {
		return false;
	}
	QString safety = m_pendingFirstContact.value(peerSession).second;
	if (safety.isEmpty()) {
		safety = safetyNumberFor(peerSession);
	}
	m_trustStore->checkAndPin(serverDigest(), user->qsName, fp, safety);
	if (verified) {
		m_trustStore->markVerified(serverDigest(), user->qsName);
	}
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
	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache.clear();
}

void FileTransferManager::refreshPinCache() {
	if (!m_trustStore || !m_identity) {
		return;
	}
	// Re-apply limits whenever the world changes; cheap and idempotent
	applyEngineConfig();
	const QByteArray digest = serverDigest();
	if (digest.isEmpty()) {
		return;
	}

	QHash< unsigned int, QByteArray > fresh;
	// Only channel peers with the file-transfer capability can ever talk to
	// us; cache every user we have a pin for.
	const ClientUser *self = ClientUser::get(Global::get().uiSession);
	if (self && self->cChannel) {
		for (const User *u : self->cChannel->qlUsers) {
			const auto *user = static_cast< const ClientUser * >(u);
			if (!user || user->uiSession == Global::get().uiSession)
				continue;
			PQFT::PinnedPeer peer;
			if (m_trustStore->lookup(peer, digest, user->qsName)) {
				fresh.insert(user->uiSession, peer.fingerprint);
			}
		}
	}

	QMutexLocker lock(&m_pinCacheMutex);
	m_pinCache = std::move(fresh);
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
