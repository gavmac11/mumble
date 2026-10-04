// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferManager.h"

#include "Channel.h"
#include "ClientUser.h"
#include "Database.h"
#include "Global.h"
#include "ServerHandler.h"

#include <QThread>

#include <algorithm>

FileTransferManager::FileTransferManager(QObject *parent) : QObject(parent) {
	qRegisterMetaType< PQFT::FTTransferInfo >("PQFT::FTTransferInfo");

	if (Global::get().db) {
		const QSqlDatabase connection = Global::get().db->connection();
		PQFT::FileTransferIdentity::ensureSchema(connection);
		m_identity	= std::make_unique< FileTransferIdentity >(connection);
		m_trustStore = std::make_unique< PeerTrustStore >(connection);
	}

	m_workerThread = std::make_unique< QThread >(this);
	m_workerThread->setObjectName(QStringLiteral("FileTransferWorker"));
	m_engine = new PQFT::FileTransferEngine();
	m_engine->moveToThread(m_workerThread.get());
	connect(m_workerThread.get(), &QThread::finished, m_engine, &QObject::deleteLater);
	m_workerThread->start();

	setupEngineTransports();
	applyEngineConfig();
}

FileTransferManager::~FileTransferManager() {
	m_workerThread->quit();
	m_workerThread->wait(3000);
}

void FileTransferManager::setupEngineTransports() {
	// The engine calls these from the worker thread; sends are marshaled to
	// the ServerHandler thread (which owns the socket).
	m_engine->setTransport(
		[](unsigned int targetSession, const QByteArray &payload) {
			auto sh = Global::get().sh;
			if (sh) {
				QMetaObject::invokeMethod(sh.get(), "sendFileTransferControl", Qt::QueuedConnection,
										  Q_ARG(QList< unsigned int >, QList< unsigned int >{ targetSession }),
										  Q_ARG(QByteArray, payload));
			}
		},
		[](const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) {
			auto sh = Global::get().sh;
			if (sh) {
				QMetaObject::invokeMethod(sh.get(), "sendFileData", Qt::QueuedConnection,
										  Q_ARG(QByteArray, transferId), Q_ARG(quint64, index),
										  Q_ARG(std::optional< quint64 >, std::optional< quint64 >(total)),
										  Q_ARG(QByteArray, data));
			}
		});

	// Engine pin lookups run on the worker thread against the cache
	PQFT::FileTransferEngine *engine = m_engine;
	QMutex *cacheMutex				 = &m_pinCacheMutex;
	QHash< unsigned int, QByteArray > *cache = &m_pinCache;
	engine->setPinLookup([cacheMutex, cache](unsigned int peerSession) -> QByteArray {
		QMutexLocker lock(cacheMutex);
		return cache->value(peerSession);
	});

	// Identity signing: FileTransferIdentity::sign only touches in-memory
	// keys, so it is safe to call from the worker.
	FileTransferIdentity *identity = m_identity.get();
	if (identity) {
		engine->setIdentity(QByteArray(), [identity](QByteArray &sig, const QByteArray &msg,
													 const QByteArray &ctx) {
			return identity->sign(sig, msg, ctx);
		});
	}

	connect(engine, &PQFT::FileTransferEngine::transferUpdated, this,
			&FileTransferManager::transferUpdated, Qt::QueuedConnection);
	connect(engine, &PQFT::FileTransferEngine::firstContact, this,
			[this](unsigned int peerSession, const QByteArray &peerFingerprint,
				   const QString &safetyNumberStr, const QByteArray &) {
				// Pin on first observation (TOFU); verification state is set
				// by the user through the safety-number dialog.
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
				emit firstContact(peerSession, peerFingerprint, safetyNumberStr);
			},
			Qt::QueuedConnection);
	connect(engine, &PQFT::FileTransferEngine::peerBlocked, this,
			[this](unsigned int peerSession) {
				const ClientUser *user = ClientUser::get(peerSession);
				emit peerBlocked(peerSession, user ? user->qsName : tr("Unknown user"));
			},
			Qt::QueuedConnection);
	connect(engine, &PQFT::FileTransferEngine::passwordRequired, this,
			[this](const QByteArray &transferId) { emit passwordRequired(transferId, QString()); },
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
	QMetaObject::invokeMethod(m_engine, [this, actor, payload]() {
				m_engine->onControlMessage(actor, payload);
			}, Qt::QueuedConnection);
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
	const quint64 index	 = msg.chunk_index();
	const quint64 hint	 = msg.has_chunk_count() ? msg.chunk_count() : 0;
	QMetaObject::invokeMethod(m_engine, [this, actor, transferId, index, hint, data]() {
				m_engine->onDataMessage(actor, transferId, index, hint, data);
			}, Qt::QueuedConnection);
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
	const QByteArray pk	  = id->publicKey();
	QMetaObject::invokeMethod(
		m_engine, [this, pk, id]() { m_engine->setIdentity(pk, [id](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) { return id->sign(sig, msg, ctx); }); },
		Qt::QueuedConnection);
}

bool FileTransferManager::startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
									const QByteArray &password,
									const QList< unsigned int > &recipients) {
	if (!hasUsableIdentity() || m_engine == nullptr || !Global::get().s.bFTEnabled) {
		return false;
	}
	QSet< unsigned int > sessions;
	for (unsigned int session : recipients) {
		sessions.insert(session);
	}
	// Refresh the public key in the engine in case the identity changed
	m_engine->setIdentity(m_identity->publicKey(), [id = m_identity.get()](QByteArray &sig,
																		  const QByteArray &msg,
																		  const QByteArray &ctx) {
		return id->sign(sig, msg, ctx);
	});

	refreshPinCache();

	const QString path = filePath;
	const QString mime = mimeType;
	const bool pwMode  = passwordMode;
	QByteArray pw	  = password;
	QMetaObject::invokeMethod(m_engine,
							  [this, path, mime, pwMode, pw, sessions]() {
								  m_engine->startSend(path, mime, pwMode, pw, sessions);
							  },
							  Qt::QueuedConnection);
	return true;
}

void FileTransferManager::abortTransfer(const QByteArray &transferId) {
	QMetaObject::invokeMethod(m_engine, [this, transferId]() { m_engine->abortTransfer(transferId); },
							  Qt::QueuedConnection);
}

void FileTransferManager::saveTransferAs(const QByteArray &transferId, const QString &targetPath) {
	QMetaObject::invokeMethod(m_engine,
							  [this, transferId, targetPath]() {
								  m_engine->saveTransferAs(transferId, targetPath);
							  },
							  Qt::QueuedConnection);
}

void FileTransferManager::providePassword(const QByteArray &transferId, const QByteArray &password) {
	QByteArray pw = password;
	QMetaObject::invokeMethod(m_engine, [this, transferId, pw]() { m_engine->providePassword(transferId, pw); },
							  Qt::QueuedConnection);
}

void FileTransferManager::resolveFirstContact(unsigned int peerSession, bool verified) {
	// GUI thread: the dialog finished. Pin first so the engine's retry hits
	// the cache, then let it continue.
	if (verified) {
		refreshPinCache();
		{
			QMutexLocker lock(&m_pinCacheMutex);
			if (!m_pinCache.contains(peerSession)) {
				// Still unknown (e.g. user left): cannot verify — treat as declined
				verified = false;
			}
		}
	}
	const bool v = verified;
	QMetaObject::invokeMethod(m_engine, [this, peerSession, v]() {
		m_engine->resolveFirstContact(peerSession, v);
	}, Qt::QueuedConnection);
}

bool FileTransferManager::pinPeer(unsigned int peerSession, bool verified) {
	const ClientUser *user = ClientUser::get(peerSession);
	if (!user || !m_trustStore) {
		return false;
	}
	// Which fingerprint? The pin cache may already carry it (receiver side
	// ran checkAndPin); for the sender-side first-use flow the engine just
	// emitted it. Look it up from the live cache first.
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
		return false;
	}
	const QString safety = safetyNumberFor(peerSession);
	m_trustStore->checkAndPin(serverDigest(), user->qsName, fp, safety);
	if (verified) {
		m_trustStore->markVerified(serverDigest(), user->qsName);
	}
	refreshPinCache();
	return true;
}

PQFT::TrustState FileTransferManager::trustStateFor(unsigned int peerSession,
													QByteArray &pinnedFingerprint) {
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
	if (m_engine) {
		QMetaObject::invokeMethod(m_engine, [this]() { m_engine->abortAll(); },
								  Qt::QueuedConnection);
	}
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
	config.chunkSize = static_cast< quint32 >(
		qBound(16, s.iFTChunkKB, 1024) * 1024);
	config.sendRateBytesPerSecond =
		static_cast< quint32 >(qBound(0, s.iFTSendPaceKiB, 1 << 20)) * 1024ull;
	config.maxReceiveSize =
		static_cast< quint64 >(qMax(1, s.iFTMaxReceiveMiB)) * 1024ull * 1024ull;
	config.handshakeTimeoutMSecs   = 10000;
	config.receiveIdleTimeoutMSecs = 60000;
	QMetaObject::invokeMethod(m_engine, [this, config]() { m_engine->setConfig(config); },
							  Qt::QueuedConnection);

	// A disabled feature stops everything in flight
	if (!s.bFTEnabled) {
		disconnectCleanup();
	}
}
