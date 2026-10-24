// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "Channel.h"
#include "ClientUser.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FTSavePolicy.h"
#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "Global.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

class TestFileTransferManager : public QObject {
	Q_OBJECT
private slots:
	void init() {
		m_serverDigest = QByteArray(20, 's');
		m_signCalls.store(0);
		m_directory = std::make_unique< QTemporaryDir >();
		QVERIFY(m_directory->isValid());
		m_global                = std::make_unique< Global >(m_directory->filePath("settings.json"));
		Global::g_global_struct = m_global.get();
		// Most cases cover the opt-in safety-number prompts; first-use cases turn this off.
		m_global->s.bFTRequireVerification = true;
		m_oldM1                 = firstContactFrame();
		m_newM1                 = firstContactFrame();
		QVERIFY(!m_oldM1.isEmpty());
		QVERIFY(!m_newM1.isEmpty());
	}

	void recipientsSkipUnavailableMembers() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		ClientUser legacy, broken;
		legacy.uiSession            = 43;
		legacy.qsName               = "Legacy";
		legacy.bFileTransferCapable = true;
		broken.uiSession            = 44;
		broken.qsName               = "Broken pin";
		broken.bFileTransferCapable = broken.bFileTransferDeliveryReceipts = true;
		m_channel->addUser(&legacy);
		m_channel->addUser(&broken);
		{
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(43, &legacy);
			ClientUser::c_qmUsers.insert(44, &broken);
		}
		const auto cleanup = qScopeGuard([&]() {
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.remove(43);
			ClientUser::c_qmUsers.remove(44);
		});
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, broken.qsName, "invalid", "broken"),
				 PQFT::TrustState::Pinned);
		bool unavailable      = false;
		const auto recipients = manager.availableSendRecipients(unavailable);
		QCOMPARE(recipients.size(), 1);
		QCOMPARE(recipients.first().session, 42u);
		QVERIFY(unavailable);
		m_trustDb.close();
		QVERIFY(manager.availableSendRecipients(unavailable).isEmpty());
		QVERIFY(unavailable);
	}

	void controlsDoNotRebuildWholePinCache() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		manager.refreshPinCache();
		{
			QMutexLocker lock(&manager.m_pinCacheMutex);
			manager.m_pinCache.insert(99, "cache sentinel");
		}
		for (int i = 0; i < 32; ++i)
			deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QMutexLocker lock(&manager.m_pinCacheMutex);
		QCOMPARE(manager.m_pinCache.value(99), QByteArray("cache sentinel"));
	}

	void unavailablePeerRecoversWithoutChannelScan() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		m_trustDb.close();
		manager.refreshPinCache();
		QVERIFY(manager.m_unavailablePeers.contains(42));
		manager.m_pinCache.insert(99, "cache sentinel");
		deliverControl(manager, m_oldM1);
		QVERIFY(manager.m_unavailablePeers.contains(42));
		QVERIFY(m_trustDb.open());
		QVERIFY(PQFT::FileTransferIdentity::ensureSchema(m_trustDb));
		// Floods cannot bypass the retry interval after the failed lookup.
		for (int i = 0; i < 32; ++i)
			deliverControl(manager, m_oldM1);
		QVERIFY(manager.m_unavailablePeers.contains(42));
		QTest::qWait(1100);
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		deliverControl(manager, m_oldM1);
		QTRY_COMPARE(prompts.count(), 1);
		QVERIFY(!manager.m_unavailablePeers.contains(42));
		QMutexLocker lock(&manager.m_pinCacheMutex);
		QCOMPARE(manager.m_pinCache.value(99), QByteArray("cache sentinel"));
	}

	void duplicateSessionPromptsAcceptSameIdentity() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		ClientUser other;
		other.uiSession            = 43;
		other.qsName               = m_peer->qsName;
		other.bFileTransferCapable = other.bFileTransferDeliveryReceipts = true;
		m_channel->addUser(&other);
		{
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(43, &other);
		}
		const auto cleanup = qScopeGuard([&]() {
			manager.disconnectCleanup();
			QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection);
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.remove(43);
		});
		manager.refreshPinCache();
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		deliverControl(manager, m_oldM1);
		MumbleProto::FileTransferControl control;
		control.set_actor(43);
		control.set_payload(m_oldM1.constData(), static_cast< size_t >(m_oldM1.size()));
		manager.handleControlMessage(control);
		QTRY_COMPARE_WITH_TIMEOUT(prompts.size(), 2, 5000);
		const auto fp    = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		const auto first = manager.firstContactToken(42), second = manager.firstContactToken(43);
		QVERIFY(first && second);
		QVERIFY(manager.resolveFirstContact(42, fp, first, true));
		QVERIFY(manager.resolveFirstContact(43, fp, second, true));
		PQFT::PinnedPeer stored;
		QVERIFY(manager.m_trustStore->lookup(stored, m_serverDigest, m_peer->qsName));
		QVERIFY(stored.verified);
		QCOMPARE(stored.fingerprint, fp);
	}

	void sendLimitUsesActualChunksAndBusyHasOwnError() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QVERIFY(manager.m_identity->createIdentity("owned-send-limit-passphrase"));
		manager.m_transportSetup = [](PQFT::FileTransferEngine &engine) {
			engine.setTransport([](unsigned int, const QByteArray &) {}, {});
		};
		m_global->s.iFTChunkKB     = 256;
		m_global->s.iFTSendPaceKiB = 0;
		MumbleProto::ServerConfig relay;
		relay.set_file_transfer_relay_version(1);
		relay.set_max_file_transfer_size(1024 * 1024);
		manager.configureRelay(relay);
		const auto selected = manager.sendRecipient(42);
		QVERIFY(selected);
		QFile source(m_directory->filePath("near-limit.bin"));
		QVERIFY(source.open(QIODevice::WriteOnly));
		QVERIFY(source.resize(1024 * 1024 - 63));
		source.close();
		QVERIFY(!manager.startSend(source.fileName(), "application/octet-stream", false, {}, { *selected }));
		QVERIFY(manager.sendAvailabilityError().contains("limit"));
		QVERIFY(source.open(QIODevice::ReadWrite));
		QVERIFY(source.resize(1024 * 1024 - 64));
		source.close();
		QVERIFY(manager.startSend(source.fileName(), "application/octet-stream", false, {}, { *selected }));
		QVERIFY(!manager.startSend(source.fileName(), "application/octet-stream", false, {}, { *selected }));
		QVERIFY(manager.sendAvailabilityError().contains(selected->name));
		QVERIFY(manager.sendAvailabilityError().contains("pending"));
		manager.disconnectCleanup();
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
	}

	void repeatedFirstContactsKeepOnePrompt() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		for (int i = 0; i < 32; ++i)
			deliverControl(manager, i % 2 ? m_newM1 : m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(prompts.count(), 1);
		QCOMPARE(manager.m_contactRequests.size(), 1);
		QVERIFY(manager.m_trustStore->list().isEmpty());
	}

	void explicitResetRejectsReplacedIdentity() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QByteArray fp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, fp, "old"),
				 PQFT::TrustState::Pinned);
		const auto shown = manager.sendRecipient(42);
		QVERIFY(shown.has_value());
		QVERIFY(manager.m_trustStore->removePin(m_serverDigest, m_peer->qsName, fp));
		const QByteArray replacement = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, replacement, "new"),
				 PQFT::TrustState::Pinned);
		QVERIFY(!manager.resetPeerTrust(*shown));
		const auto current = manager.sendRecipient(42);
		QVERIFY(current.has_value());
		QCOMPARE(current->fingerprint, replacement);
		QVERIFY(manager.resetPeerTrust(*current));
		const auto reset = manager.sendRecipient(42);
		QVERIFY(reset.has_value());
		QCOMPARE(reset->trust, PQFT::TrustState::NewPeer);
		QVERIFY(reset->fingerprint.isEmpty());
	}

	void resettingOnePeerPreservesOtherPrompt() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		ClientUser other;
		other.uiSession            = 43;
		other.qsName               = QStringLiteral("Other private peer");
		other.bFileTransferCapable = other.bFileTransferDeliveryReceipts = true;
		m_channel->addUser(&other);
		{
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(43, &other);
		}
		const auto cleanup = qScopeGuard([&]() {
			manager.disconnectCleanup();
			QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection);
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.remove(43);
		});
		manager.refreshPinCache(); // The joined peer triggers a production user-list refresh.
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		QSignalSpy cancelled(&manager, &FileTransferManager::firstContactCancelled);
		deliverControl(manager, m_oldM1);
		MumbleProto::FileTransferControl message;
		message.set_actor(43);
		message.set_payload(m_newM1.constData(), static_cast< size_t >(m_newM1.size()));
		manager.handleControlMessage(message);
		QTRY_COMPARE_WITH_TIMEOUT(prompts.size(), 2, 5000);
		const quint64 firstToken = manager.firstContactToken(42);
		const quint64 otherToken = manager.firstContactToken(43);
		QVERIFY(firstToken && otherToken && firstToken != otherToken);
		const QByteArray fp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, fp, "reset fixture"),
				 PQFT::TrustState::Pinned);
		const auto shown = manager.sendRecipient(42);
		QVERIFY(shown);
		QVERIFY(manager.resetPeerTrust(*shown));
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(manager.m_contactRequests.size(), 1);
		QCOMPARE(manager.firstContactToken(43), otherToken);
		QVERIFY(!cancelled.isEmpty());
		for (const auto &event : cancelled)
			QVERIFY(event.first().toULongLong() != otherToken);
		QVERIFY(!manager.resolveFirstContact(43, PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1)),
											 otherToken, false));
		QVERIFY(manager.m_contactRequests.isEmpty());
		QVERIFY(manager.m_trustStore->list().isEmpty());
	}

	void peerErrorCannotFailOutgoingTransfer() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QVERIFY(manager.m_identity->createIdentity(QStringLiteral("isolated-error-routing-passphrase")));
		manager.m_transportSetup = [](PQFT::FileTransferEngine &engine) {
			engine.setTransport([](unsigned int, const QByteArray &) {}, {});
		};
		MumbleProto::ServerConfig relay;
		relay.set_file_transfer_relay_version(1);
		manager.configureRelay(relay);
		const auto selected = manager.sendRecipient(42);
		QVERIFY(selected);
		QFile source(m_directory->filePath("error-routing.bin"));
		QVERIFY(source.open(QIODevice::WriteOnly));
		QCOMPARE(source.write(QByteArray(32768, 'e')), qint64(32768));
		source.close();
		QSignalSpy updates(&manager, &FileTransferManager::transferUpdated);
		QVERIFY(manager.startSend(source.fileName(), "application/octet-stream", false, {}, { *selected }));
		QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
		const QByteArray id = updates.last().first().value< PQFT::FTTransferInfo >().transferId;
		QCOMPARE(id.size(), 16);
		MumbleProto::FileData error;
		error.set_actor(42);
		error.set_transfer_id(id.constData(), static_cast< size_t >(id.size()));
		error.set_data("");
		error.set_relay_error("injected peer error");
		manager.handleDataMessage(error);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		for (const auto &event : updates)
			QVERIFY(event.first().value< PQFT::FTTransferInfo >().state != PQFT::FTTransferInfo::State::Failed);
		error.set_actor(41);
		manager.handleDataMessage(error);
		QTRY_VERIFY_WITH_TIMEOUT(
			updates.last().first().value< PQFT::FTTransferInfo >().state == PQFT::FTTransferInfo::State::Failed, 5000);
		QCOMPARE(updates.last().first().value< PQFT::FTTransferInfo >().error, QStringLiteral("injected peer error"));
	}

	void cleanup() {
		if (m_peer) {
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.remove(m_peer->uiSession);
			ClientUser::c_qmUsers.remove(m_self->uiSession);
		}
		m_peer.reset();
		m_replacement.reset();
		m_self.reset();
		m_channel.reset();
		if (m_trustDb.isValid()) {
			m_trustDb.close();
			m_trustDb = QSqlDatabase();
			QSqlDatabase::removeDatabase(QStringLiteral("presented-key-private-fixture"));
		}
		m_global.reset();
		Global::g_global_struct = nullptr;
		m_directory.reset();
	}

	void changedPresentedKeyDoesNotVerifyCachedPin() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QByteArray oldFp   = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		const QByteArray shownFp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, oldFp, QStringLiteral("old safety")),
				 PQFT::TrustState::Pinned);
		manager.m_pinCache.insert(42, oldFp);
		manager.m_pendingFirstContact.insert(
			42, FileTransferManager::PendingFirstContact{ shownFp,
														  QStringLiteral("shown safety"),
														  QPointer< ClientUser >(m_peer.get()),
														  m_peer->qsName,
														  m_serverDigest,
														  0,
														  {},
														  0,
														  false,
														  0,
														  0 });
		const bool accepted = manager.pinPeer(42, shownFp, true);
		PQFT::PinnedPeer stored;
		QVERIFY(manager.m_trustStore->lookup(stored, m_serverDigest, m_peer->qsName));
		QCOMPARE(stored.fingerprint, oldFp);
		QVERIFY2(!stored.verified, "Accepting a changed presented key verified the previously cached key instead");
		QVERIFY(!accepted);
	}

	void joinedPeerChangedKeyIsBlockedBeforeDialog() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QByteArray oldFp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, oldFp, QStringLiteral("old safety")),
				 PQFT::TrustState::Pinned);
		// Reproduce a peer joining after the session cache was built: the database already knows them.
		QVERIFY(manager.m_pinCache.isEmpty());
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		QSignalSpy blocked(&manager, &FileTransferManager::peerBlocked);
		deliverControl(manager, m_newM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(contact.size(), 0);
		QCOMPARE(blocked.size(), 1);
		PQFT::PinnedPeer stored;
		QVERIFY(manager.m_trustStore->lookup(stored, m_serverDigest, m_peer->qsName));
		QCOMPARE(stored.fingerprint, oldFp);
		QVERIFY(!stored.verified);
	}

	void displayedKeyIsPinnedAndVerified() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		deliverControl(manager, m_newM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		const QByteArray shown = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		QVERIFY(manager.resolveFirstContact(42, shown, manager.firstContactToken(42), true));
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCOMPARE(m_signCalls.load(), 1);
		PQFT::PinnedPeer stored;
		QVERIFY(manager.m_trustStore->lookup(stored, m_serverDigest, m_peer->qsName));
		QCOMPARE(stored.fingerprint, shown);
		QVERIFY(stored.verified);
		QCOMPARE(manager.m_pinCache.value(42), shown);
	}

	void joinedMatchingPeerKeepsVerificationWithoutDialog() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QByteArray fp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, fp, QStringLiteral("known safety")),
				 PQFT::TrustState::Pinned);
		QVERIFY(manager.m_trustStore->markVerified(m_serverDigest, m_peer->qsName, fp));
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		QSignalSpy blocked(&manager, &FileTransferManager::peerBlocked);
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(contact.size(), 0);
		QCOMPARE(blocked.size(), 0);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCOMPARE(m_signCalls.load(), 1);
		QCOMPARE(manager.m_pinCache.value(42), fp);
		PQFT::PinnedPeer stored;
		QVERIFY(manager.m_trustStore->lookup(stored, m_serverDigest, m_peer->qsName));
		QCOMPARE(stored.fingerprint, fp);
		QVERIFY(stored.verified);
	}

	void renamedPeerDoesNotBorrowAnotherKeysVerification() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QString originalName = m_peer->qsName;
		const QByteArray actualFp  = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		const QByteArray otherFp   = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, originalName, actualFp, "original"),
				 PQFT::TrustState::Pinned);
		const QString otherName = QStringLiteral("Historically verified peer");
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, otherName, otherFp, "other"),
				 PQFT::TrustState::Pinned);
		QVERIFY(manager.m_trustStore->markVerified(m_serverDigest, otherName, otherFp));
		m_peer->qsName                  = otherName;
		m_global->s.bFTAutoAcceptPinned = true;
		m_global->s.qsFTDownloadDir     = m_directory->path();
		PQFT::FTTransferInfo info;
		info.peerSession = 42;
		info.incoming    = true;
		info.state       = PQFT::FTTransferInfo::State::Ready;
		info.fileName    = QStringLiteral("safe.txt");
		QByteArray storedFp;
		info.peerName        = originalName;
		info.peerFingerprint = actualFp;
		const auto trust     = manager.trustStateForTransfer(info, storedFp);
		QCOMPARE(trust, PQFT::TrustState::Pinned);
		QCOMPARE(storedFp, actualFp);
		QVERIFY2(PQFT::automaticSaveTarget(m_global->s, info, trust).isEmpty(),
				 "A renamed sender borrowed a different key's verification for automatic saving");
	}

	void incomingReadyUsesCapturedIdentity_data() {
		QTest::addColumn< QString >("change");
		for (const char *change :
			 { "rename", "disconnect", "changed-key", "removed-pin", "missing-name", "missing-key" })
			QTest::newRow(change) << QString::fromLatin1(change);
	}
	void incomingReadyUsesCapturedIdentity() {
		QFETCH(QString, change);
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		PQFT::FTTransferInfo info;
		info.peerName        = m_peer->qsName;
		info.peerFingerprint = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		info.peerSession     = 42;
		info.incoming        = true;
		info.transferId      = QByteArray(PQFT::TransferIdSize, 't');
		info.state           = PQFT::FTTransferInfo::State::Ready;
		info.fileName        = QStringLiteral("safe.txt");
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, info.peerName, info.peerFingerprint, "original"),
				 PQFT::TrustState::Pinned);
		QVERIFY(manager.m_trustStore->markVerified(m_serverDigest, info.peerName, info.peerFingerprint));
		if (change == QLatin1String("rename"))
			m_peer->qsName = QStringLiteral("New live name");
		else if (change == QLatin1String("disconnect")) {
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.remove(42);
		} else if (change == QLatin1String("missing-name"))
			info.peerName.clear();
		else if (change == QLatin1String("missing-key"))
			info.peerFingerprint.clear();
		else {
			QVERIFY(manager.m_trustStore->removePin(m_serverDigest, info.peerName, info.peerFingerprint));
			if (change == QLatin1String("changed-key")) {
				const auto other = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
				QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, info.peerName, other, "replacement"),
						 PQFT::TrustState::Pinned);
			}
		}
		QSignalSpy updates(&manager, &FileTransferManager::transferUpdated);
		QSignalSpy blocked(&manager, &FileTransferManager::peerBlocked);
		QVERIFY(QMetaObject::invokeMethod(
			manager.m_engine, [&manager, info]() { emit manager.m_engine->transferUpdated(info); },
			Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(updates.size(), 1);
		const auto forwarded     = updates.first().first().value< PQFT::FTTransferInfo >();
		const bool expectedReady = change == QLatin1String("rename") || change == QLatin1String("disconnect");
		QCOMPARE(forwarded.state,
				 expectedReady ? PQFT::FTTransferInfo::State::Ready : PQFT::FTTransferInfo::State::Failed);
		QCOMPARE(blocked.size(), change == QLatin1String("changed-key") ? 1 : 0);
		QCOMPARE(forwarded.peerName, info.peerName);
		QCOMPARE(forwarded.peerFingerprint, info.peerFingerprint);
	}

	void senderSelectionRechecksShownIdentity_data() {
		QTest::addColumn< QString >("change");
		for (const char *change : { "unchanged", "rename", "key", "removed-pin", "verification", "database-error",
									"connection", "capability", "session-reuse", "channel" })
			QTest::newRow(change) << QString::fromLatin1(change);
	}
	void senderSelectionRechecksShownIdentity() {
		QFETCH(QString, change);
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const QByteArray fp = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1));
		QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, fp, "shown"),
				 PQFT::TrustState::Pinned);
		QVERIFY(manager.m_trustStore->markVerified(m_serverDigest, m_peer->qsName, fp));
		const auto shown = manager.sendRecipient(42);
		QVERIFY(shown);
		QCOMPARE(shown->fingerprint, fp);
		QCOMPARE(shown->trust, PQFT::TrustState::Verified);
		if (change == QLatin1String("rename")) {
			const auto other = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
			m_peer->qsName   = QStringLiteral("Other verified name");
			QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, other, "other"),
					 PQFT::TrustState::Pinned);
			QVERIFY(manager.m_trustStore->markVerified(m_serverDigest, m_peer->qsName, other));
		} else if (change == QLatin1String("key") || change == QLatin1String("removed-pin")
				   || change == QLatin1String("verification")) {
			QVERIFY(manager.m_trustStore->removePin(m_serverDigest, m_peer->qsName, fp));
			if (change != QLatin1String("removed-pin")) {
				const auto other = change == QLatin1String("key")
									   ? PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1))
									   : fp;
				QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, other, "replacement"),
						 PQFT::TrustState::Pinned);
			}
		} else if (change == QLatin1String("database-error"))
			m_trustDb.close();
		else if (change == QLatin1String("connection"))
			manager.disconnectCleanup();
		else if (change == QLatin1String("capability"))
			m_peer->bFileTransferCapable = false;
		else if (change == QLatin1String("channel")) {
			m_channel->removeUser(m_peer.get());
			m_peer->cChannel = nullptr;
		} else if (change == QLatin1String("session-reuse")) {
			m_replacement                       = std::make_unique< ClientUser >();
			m_replacement->uiSession            = 42;
			m_replacement->qsName               = m_peer->qsName;
			m_replacement->bFileTransferCapable = true;
			m_replacement->bFileTransferDeliveryReceipts = true;
			m_replacement->cChannel             = m_channel.get();
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(42, m_replacement.get());
		}
		QCOMPARE(manager.sendRecipientStillCurrent(*shown), change == QLatin1String("unchanged"));
	}

	void senderFirstUseRequiresReadableDatabase() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		const auto firstUse = manager.sendRecipient(42);
		QVERIFY(firstUse);
		QCOMPARE(firstUse->trust, PQFT::TrustState::NewPeer);
		QVERIFY(firstUse->fingerprint.isEmpty());
		m_trustDb.close();
		QVERIFY(!manager.sendRecipient(42));
		QVERIFY(!manager.sendRecipientStillCurrent(*firstUse));
	}

	void productionSenderAuthorization_data() {
		QTest::addColumn< QString >("decision");
		for (const char *decision :
			 { "accept", "decline", "rename", "changed-pin", "storage-error", "old-relay", "first-use" })
			QTest::newRow(decision) << QString::fromLatin1(decision);
	}
	void productionSenderAuthorization() {
		QFETCH(QString, decision);
		const bool firstUse                = decision == QLatin1String("first-use");
		m_global->s.bFTRequireVerification = !firstUse;
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QVERIFY(manager.m_identity->createIdentity(QStringLiteral("isolated-sender-passphrase")));
		PQFT::FileTransferEngine receiver;
		PQFT::SigMLDSA65 signature;
		QByteArray receiverPublic;
		PQFT::SecureBytes receiverSecret;
		QVERIFY(signature.keypair(receiverPublic, receiverSecret));
		receiver.setIdentity(receiverPublic,
							 [&](QByteArray &out, const QByteArray &message, const QByteArray &context) {
								 return signature.sign(out, receiverSecret, message, context);
							 });
		const QByteArray senderFp = manager.m_identity->fingerprint();
		receiver.setPinLookup([senderFp](unsigned int) { return senderFp; });
		bool holdM4 = true;
		QList< QByteArray > pendingResponses;
		receiver.setTransport(
			[&](unsigned int, const QByteArray &frame) {
				if (holdM4 && receiverPublic.size() > 0 && frame.size() > 0
					&& (static_cast< unsigned char >(frame[0]) & 15) == 4)
					pendingResponses.append(frame);
				else
					deliverControl(manager, frame);
			},
			{});
		int chunks               = 0;
		manager.m_transportSetup = [&](PQFT::FileTransferEngine &engine) {
			engine.setTransport(
				[&](unsigned int, const QByteArray &frame) {
					QMetaObject::invokeMethod(
						&receiver, [&receiver, frame]() { receiver.onControlMessage(41, frame); },
						Qt::QueuedConnection);
				},
				{});
			engine.setTargetedChunkTransport([&](const QList< unsigned int > &targets, const QByteArray &id,
												 quint64 index, quint64 total, const QByteArray &data) {
				QMetaObject::invokeMethod(
					&receiver,
					[&, targets, id, index, total, data]() {
						QCOMPARE(targets, QList< unsigned int >{ 42 });
						++chunks;
						receiver.onDataMessage(41, id, index, total, data);
					},
					Qt::QueuedConnection);
				return true;
			});
		};
		MumbleProto::ServerConfig relay;
		relay.set_file_transfer_relay_version(decision == QLatin1String("old-relay") ? 0 : 1);
		relay.set_file_transfer_rate(2 * 1024 * 1024);
		manager.configureRelay(relay);
		const auto selected = manager.sendRecipient(42);
		QVERIFY(selected);
		const QString source = m_directory->filePath("sender.bin");
		QFile file(source);
		QVERIFY(file.open(QIODevice::WriteOnly));
		QCOMPARE(file.write(QByteArray(64 * 1024, 's')), qint64(64 * 1024));
		file.close();
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		QSignalSpy updates(&manager, &FileTransferManager::transferUpdated);
		const auto cleanup = qScopeGuard([&]() {
			manager.disconnectCleanup();
			QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection);
		});
		QCOMPARE(manager.startSend(source, "application/octet-stream", false, {}, { *selected }),
				 decision != QLatin1String("old-relay"));
		if (decision == QLatin1String("old-relay")) {
			QCOMPARE(chunks, 0);
			return;
		}
		QTRY_COMPARE_WITH_TIMEOUT(pendingResponses.size(), 1, 10000);
		QCOMPARE(chunks, 0);
		if (decision == QLatin1String("rename"))
			m_peer->qsName = QStringLiteral("Different name after selection");
		else if (decision == QLatin1String("changed-pin"))
			QCOMPARE(manager.m_trustStore->checkAndPin(m_serverDigest, m_peer->qsName, QByteArray(PQFT::HashSize, 'x'),
													   "changed"),
					 PQFT::TrustState::Pinned);
		else if (decision == QLatin1String("storage-error"))
			m_trustDb.close();
		holdM4 = false;
		deliverControl(manager, pendingResponses.takeFirst());
		if (decision == QLatin1String("accept") || decision == QLatin1String("decline")) {
			QTRY_COMPARE_WITH_TIMEOUT(prompts.size(), 1, 10000);
			QCOMPARE(chunks, 0);
			QVERIFY(manager.m_trustStore->list().isEmpty());
			QCOMPARE(manager.resolveFirstContact(42, PQFT::identityFingerprint(receiverPublic),
												 manager.firstContactToken(42), decision == QLatin1String("accept")),
					 decision == QLatin1String("accept"));
		}
		const auto terminal = [&]() {
			for (const auto &event : updates) {
				const auto info = event.first().value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Saved
					|| info.state == PQFT::FTTransferInfo::State::Failed)
					return info.state;
			}
			return PQFT::FTTransferInfo::State::Handshaking;
		};
		QTRY_VERIFY_WITH_TIMEOUT(terminal() != PQFT::FTTransferInfo::State::Handshaking, 10000);
		const bool delivered = decision == QLatin1String("accept") || firstUse;
		QCOMPARE(terminal(), delivered ? PQFT::FTTransferInfo::State::Saved : PQFT::FTTransferInfo::State::Failed);
		QCOMPARE(chunks > 0, delivered);
		if (firstUse)
			QCOMPARE(prompts.size(), 0);
		if (decision != QLatin1String("storage-error")) {
			PQFT::PinnedPeer pin;
			if (delivered) {
				// First use pins the key M4 authenticated, but only a dialog marks it verified.
				QVERIFY(manager.m_trustStore->lookup(pin, m_serverDigest, selected->name));
				QCOMPARE(pin.fingerprint, PQFT::identityFingerprint(receiverPublic));
				QCOMPARE(pin.verified, !firstUse);
			} else if (decision != QLatin1String("changed-pin"))
				QVERIFY(manager.m_trustStore->list().isEmpty());
		}
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		manager.disconnectCleanup();
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
	}

	void receivedFileRechecksPinBeforeSave_data() {
		QTest::addColumn< QString >("change");
		QTest::newRow("verified") << QStringLiteral("verified");
		QTest::newRow("pinned") << QStringLiteral("pinned");
		QTest::newRow("removed") << QStringLiteral("removed");
		QTest::newRow("changed") << QStringLiteral("changed");
		QTest::newRow("storage-unavailable") << QStringLiteral("storage-unavailable");
		QTest::newRow("save-retry") << QStringLiteral("save-retry");
		QTest::newRow("duplicate-save") << QStringLiteral("duplicate-save");
		QTest::newRow("no-trust-store") << QStringLiteral("no-trust-store");
		QTest::newRow("removed-disconnect") << QStringLiteral("removed-disconnect");
	}
	void receivedFileRechecksPinBeforeSave() {
		QFETCH(QString, change);
		PQFT::SigMLDSA65 sig;
		QByteArray senderPublic;
		PQFT::SecureBytes senderSecret;
		QVERIFY(sig.keypair(senderPublic, senderSecret));
		QSemaphore entered, release;
		// The receiver manager joins its worker before the sender and its signing key are destroyed.
		PQFT::FileTransferEngine sender;
		FileTransferManager receiver;
		QVERIFY(setupTrustFixture(receiver));
		QVERIFY(receiver.m_identity->createIdentity(QStringLiteral("owned-manager-roundtrip-passphrase")));
		QVERIFY(receiver.m_identity->isUnlocked());
		const QByteArray senderFp = PQFT::identityFingerprint(senderPublic);
		const QString name        = m_peer->qsName;
		QCOMPARE(receiver.m_trustStore->checkAndPin(m_serverDigest, name, senderFp, "owned safety"),
				 PQFT::TrustState::Pinned);
		if (change != QLatin1String("pinned"))
			QVERIFY(receiver.m_trustStore->markVerified(m_serverDigest, name, senderFp));
		receiver.refreshPinCache();
		receiver.pushIdentityToEngine();
		QVERIFY(QMetaObject::invokeMethod(
			receiver.m_engine,
			[&]() {
				receiver.m_engine->setTransport(
					[&sender](unsigned int, const QByteArray &frame) {
						QMetaObject::invokeMethod(
							&sender, [&sender, frame]() { sender.onControlMessage(41, frame); }, Qt::QueuedConnection);
					},
					{});
			},
			Qt::BlockingQueuedConnection));
		sender.setIdentity(senderPublic, [&sig, &senderSecret](QByteArray &signature, const QByteArray &message,
															   const QByteArray &context) {
			return sig.sign(signature, senderSecret, message, context);
		});
		sender.setTransport([&receiver](unsigned int, const QByteArray &frame) { deliverControl(receiver, frame); },
							[&receiver](const QByteArray &id, quint64 index, quint64 count, const QByteArray &data) {
								MumbleProto::FileData message;
								message.set_actor(42);
								message.set_transfer_id(id.constData(), static_cast< size_t >(id.size()));
								message.set_chunk_index(index);
								message.set_chunk_count(count);
								message.set_data(data.constData(), static_cast< size_t >(data.size()));
								receiver.handleDataMessage(message);
							});
		QFile input(m_directory->filePath("owned-source.bin"));
		QVERIFY(input.open(QIODevice::WriteOnly));
		const QByteArray bytes(32768, 's');
		QCOMPARE(input.write(bytes), static_cast< qint64 >(bytes.size()));
		input.close();
		QSignalSpy updates(&receiver, &FileTransferManager::transferUpdated);
		QVERIFY(sender.startSend(input.fileName(), "application/octet-stream", false, {},
								 { { 41, receiver.m_identity->fingerprint() } })
				> 0);
		QByteArray transfer;
		QTRY_VERIFY_WITH_TIMEOUT(
			[&]() {
				for (const auto &args : updates) {
					const auto info = args.first().value< PQFT::FTTransferInfo >();
					if (info.state == PQFT::FTTransferInfo::State::Ready) {
						transfer = info.transferId;
						return info.peerName == name && info.peerFingerprint == senderFp;
					}
				}
				return false;
			}(),
			5000);
		if (change == QLatin1String("storage-unavailable"))
			m_trustDb.close();
		if (change == QLatin1String("no-trust-store"))
			receiver.m_trustStore.reset();
		if (change == QLatin1String("removed") || change == QLatin1String("changed")
			|| change == QLatin1String("removed-disconnect")) {
			QVERIFY(receiver.m_trustStore->removePin(m_serverDigest, name, senderFp));
			if (change == QLatin1String("changed")) {
				const QByteArray other = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
				QCOMPARE(receiver.m_trustStore->checkAndPin(m_serverDigest, name, other, "replacement"),
						 PQFT::TrustState::Pinned);
			}
		}
		updates.clear();
		QSignalSpy blocked(&receiver, &FileTransferManager::peerBlocked);
		QString target = m_directory->filePath("owned-saved.bin");
		if (change == QLatin1String("save-retry")) {
			QFile existing(target);
			QVERIFY(existing.open(QIODevice::WriteOnly));
			QCOMPARE(existing.write("preserve existing"), qint64(17));
			existing.close();
			receiver.saveTransferAs(transfer, target);
			QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
			const auto retry = updates.last().first().value< PQFT::FTTransferInfo >();
			QCOMPARE(retry.state, PQFT::FTTransferInfo::State::Ready);
			QVERIFY(!retry.error.isEmpty());
			QVERIFY(existing.open(QIODevice::ReadOnly));
			QCOMPARE(existing.readAll(), QByteArray("preserve existing"));
			existing.close();
			updates.clear();
			target = m_directory->filePath("owned-retry.bin");
		}
		if (change == QLatin1String("duplicate-save")) {
			QVERIFY(QMetaObject::invokeMethod(
				receiver.m_engine,
				[&]() {
					entered.release();
					release.tryAcquire(1, 3000);
				},
				Qt::QueuedConnection));
			QVERIFY(entered.tryAcquire(1, 2000));
		}
		receiver.saveTransferAs(transfer, target);
		if (change == QLatin1String("removed-disconnect"))
			receiver.disconnectCleanup();
		if (change == QLatin1String("duplicate-save")) {
			const bool removed = receiver.m_trustStore->removePin(m_serverDigest, name, senderFp);
			receiver.saveTransferAs(transfer, m_directory->filePath("owned-duplicate.bin"));
			release.release();
			QVERIFY(removed);
		}
		QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
		// Drain actual worker cleanup and GUI forwarding so Aborted cannot hide the rejection.
		QVERIFY(QMetaObject::invokeMethod(receiver.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
		const bool expectedSaved = change == QLatin1String("verified") || change == QLatin1String("pinned")
								   || change == QLatin1String("save-retry")
								   || change == QLatin1String("duplicate-save");
		QCOMPARE(QFile::exists(target), expectedSaved);
		if (expectedSaved) {
			QFile output(target);
			QVERIFY(output.open(QIODevice::ReadOnly));
			QCOMPARE(output.readAll(), bytes);
			QCOMPARE(updates.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
		} else {
			const auto failed = updates.last().first().value< PQFT::FTTransferInfo >();
			QCOMPARE(failed.state, PQFT::FTTransferInfo::State::Failed);
			QVERIFY(!failed.error.isEmpty());
			if (change != QLatin1String("changed"))
				QCOMPARE(
					failed.error,
					QStringLiteral("Cannot confirm the sender's saved identity. Verify the sender and try again."));
		}
		QCOMPARE(blocked.size(), change == QLatin1String("changed") ? 1 : 0);
		QVERIFY(receiver.m_rejectedReceived.isEmpty());
		QVERIFY(receiver.m_savesInFlight.isEmpty());
		QVERIFY(!QFile::exists(m_directory->filePath("owned-duplicate.bin")));
	}

	void firstUseM1AloneDoesNotPin() {
		m_global->s.bFTRequireVerification = false;
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QSignalSpy prompts(&manager, &FileTransferManager::firstContact);
		deliverControl(manager, m_oldM1);
		// The handshake proceeds without a prompt (the fixture refuses to sign M2)...
		QTRY_COMPARE_WITH_TIMEOUT(m_signCalls.load(), 1, 5000);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(prompts.size(), 0);
		// ...but an unauthenticated M1 key claim is never written to the trust store.
		QVERIFY(manager.m_trustStore->list().isEmpty());
	}

	void receivedFileFirstUse_data() {
		QTest::addColumn< QString >("existing");
		QTest::newRow("new-peer") << QStringLiteral("none");
		QTest::newRow("changed-key") << QStringLiteral("changed");
	}
	void receivedFileFirstUse() {
		QFETCH(QString, existing);
		m_global->s.bFTRequireVerification = false;
		PQFT::SigMLDSA65 sig;
		QByteArray senderPublic;
		PQFT::SecureBytes senderSecret;
		QVERIFY(sig.keypair(senderPublic, senderSecret));
		PQFT::FileTransferEngine sender;
		FileTransferManager receiver;
		QVERIFY(setupTrustFixture(receiver));
		QVERIFY(receiver.m_identity->createDeviceIdentity());
		const QByteArray senderFp = PQFT::identityFingerprint(senderPublic);
		const QString name        = m_peer->qsName;
		const QByteArray other    = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		if (existing == QLatin1String("changed"))
			QCOMPARE(receiver.m_trustStore->checkAndPin(m_serverDigest, name, other, "earlier key"),
					 PQFT::TrustState::Pinned);
		receiver.refreshPinCache();
		receiver.pushIdentityToEngine();
		QVERIFY(QMetaObject::invokeMethod(
			receiver.m_engine,
			[&]() {
				receiver.m_engine->setTransport(
					[&sender](unsigned int, const QByteArray &frame) {
						QMetaObject::invokeMethod(
							&sender, [&sender, frame]() { sender.onControlMessage(41, frame); }, Qt::QueuedConnection);
					},
					{});
			},
			Qt::BlockingQueuedConnection));
		sender.setIdentity(senderPublic, [&sig, &senderSecret](QByteArray &signature, const QByteArray &message,
															   const QByteArray &context) {
			return sig.sign(signature, senderSecret, message, context);
		});
		sender.setTransport([&receiver](unsigned int, const QByteArray &frame) { deliverControl(receiver, frame); },
							[&receiver](const QByteArray &id, quint64 index, quint64 count, const QByteArray &data) {
								MumbleProto::FileData message;
								message.set_actor(42);
								message.set_transfer_id(id.constData(), static_cast< size_t >(id.size()));
								message.set_chunk_index(index);
								message.set_chunk_count(count);
								message.set_data(data.constData(), static_cast< size_t >(data.size()));
								receiver.handleDataMessage(message);
							});
		QFile input(m_directory->filePath("first-use-source.bin"));
		QVERIFY(input.open(QIODevice::WriteOnly));
		const QByteArray bytes(32768, 'f');
		QCOMPARE(input.write(bytes), static_cast< qint64 >(bytes.size()));
		input.close();
		QSignalSpy prompts(&receiver, &FileTransferManager::firstContact);
		QSignalSpy blocked(&receiver, &FileTransferManager::peerBlocked);
		QSignalSpy updates(&receiver, &FileTransferManager::transferUpdated);
		QVERIFY(sender.startSend(input.fileName(), "application/octet-stream", false, {},
								 { { 41, receiver.m_identity->fingerprint() } })
				> 0);
		if (existing == QLatin1String("changed")) {
			// A pinned key that differs is still hard-blocked, prompt or not.
			QTRY_COMPARE_WITH_TIMEOUT(blocked.size(), 1, 5000);
			QCOMPARE(prompts.size(), 0);
			PQFT::PinnedPeer pin;
			QVERIFY(receiver.m_trustStore->lookup(pin, m_serverDigest, name));
			QCOMPARE(pin.fingerprint, other);
			return;
		}
		QByteArray transfer;
		QTRY_VERIFY_WITH_TIMEOUT(
			[&]() {
				for (const auto &args : updates) {
					const auto info = args.first().value< PQFT::FTTransferInfo >();
					if (info.state == PQFT::FTTransferInfo::State::Ready) {
						transfer = info.transferId;
						return true;
					}
				}
				return false;
			}(),
			5000);
		QCOMPARE(prompts.size(), 0);
		PQFT::PinnedPeer pin;
		QVERIFY(receiver.m_trustStore->lookup(pin, m_serverDigest, name));
		QCOMPARE(pin.fingerprint, senderFp);
		QVERIFY(!pin.verified);
		QVERIFY(!receiver.isVerifiedPeer(m_peer.get()));

		updates.clear();
		const QString target = m_directory->filePath("first-use-saved.bin");
		receiver.saveTransferAs(transfer, target);
		QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
		QCOMPARE(updates.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
		QFile output(target);
		QVERIFY(output.open(QIODevice::ReadOnly));
		QCOMPARE(output.readAll(), bytes);

		// Verifying later (user menu) marks the pin verified and updates the cache.
		QSignalSpy trust(&receiver, &FileTransferManager::trustChanged);
		const auto shown = receiver.sendRecipient(42);
		QVERIFY(shown);
		QCOMPARE(shown->trust, PQFT::TrustState::Pinned);
		QVERIFY(receiver.markPeerVerified(*shown));
		QVERIFY(receiver.m_trustStore->lookup(pin, m_serverDigest, name));
		QVERIFY(pin.verified);
		QVERIFY(receiver.isVerifiedPeer(m_peer.get()));
		QCOMPARE(trust.size(), 1);
		QVERIFY(!receiver.markPeerVerified(*shown)); // stale: trust is now Verified
	}

	void changedDialogContextDoesNotPin_data() {
		QTest::addColumn< QString >("change");
		for (const char *change : { "rename", "connection", "session-reuse", "channel", "capability", "disabled" })
			QTest::newRow(change) << QString::fromLatin1(change);
	}
	void changedDialogContextDoesNotPin() {
		QFETCH(QString, change);
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		deliverControl(manager, m_newM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		const QByteArray shown = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		if (change == QLatin1String("rename"))
			m_peer->qsName = QStringLiteral("Different peer name");
		else if (change == QLatin1String("connection"))
			m_serverDigest.fill('x');
		else if (change == QLatin1String("session-reuse")) {
			m_replacement            = std::make_unique< ClientUser >();
			m_replacement->uiSession = 42;
			m_replacement->qsName    = m_peer->qsName;
			m_replacement->cChannel  = m_channel.get();
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(42, m_replacement.get());
		} else if (change == QLatin1String("channel")) {
			m_channel->removeUser(m_peer.get());
			m_peer->cChannel = nullptr;
		} else if (change == QLatin1String("capability"))
			m_peer->bFileTransferCapable = false;
		else
			m_global->s.bFTEnabled = false;
		QVERIFY(!manager.resolveFirstContact(42, shown, manager.firstContactToken(42), true));
		QVERIFY(manager.m_trustStore->list().isEmpty());
	}

	void staleDialogCannotDeclineNewPrompt_data() {
		QTest::addColumn< bool >("sameKey");
		QTest::newRow("different-key") << false;
		QTest::newRow("same-key") << true;
	}
	void staleDialogCannotDeclineNewPrompt() {
		QFETCH(bool, sameKey);
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		const QByteArray oldShown = manager.m_pendingFirstContact.value(42).fingerprint;
		const quint64 oldToken    = manager.firstContactToken(42);
		// Retire the old parked handshake, as expiry does, while its GUI dialog is still open.
		QVERIFY(QMetaObject::invokeMethod(
			manager.m_engine,
			[&manager]() {
				manager.m_engine->resolveFirstContact(42, false, manager.m_pendingFirstContact.value(42).workerToken);
			},
			Qt::BlockingQueuedConnection));
		deliverControl(manager, sameKey ? m_oldM1 : m_newM1);
		QVERIFY(QMetaObject::invokeMethod(manager.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		const QByteArray newShown = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(sameKey ? m_oldM1 : m_newM1));
		QCOMPARE(manager.m_pendingFirstContact.value(42).fingerprint, newShown);
		QVERIFY(!manager.resolveFirstContact(42, oldShown, oldToken, false));
		QCOMPARE(manager.m_pendingFirstContact.value(42).fingerprint, newShown);
		QVERIFY(!manager.resolveFirstContact(42, newShown, manager.firstContactToken(42), false));
		QVERIFY(manager.m_pendingFirstContact.isEmpty());
		QVERIFY(manager.m_trustStore->list().isEmpty());
	}

	void currentWorkerTrustPromptReachesGui() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		bool onGuiThread = false;
		connect(&manager, &FileTransferManager::firstContact, this,
				[&onGuiThread]() { onGuiThread = QThread::currentThread() == qApp->thread(); });
		PQFT::FileTransferEngine *engine = manager.m_engine;
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(engine, []() {}, Qt::BlockingQueuedConnection));
		QCOMPARE(contact.size(), 0);
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(contact.size(), 1);
		QVERIFY(onGuiThread);
		QCOMPARE(manager.m_pendingFirstContact.value(42).fingerprint,
				 PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_oldM1)));
	}

	void disconnectRejectsQueuedTrustEvents() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		QSignalSpy blocked(&manager, &FileTransferManager::peerBlocked);
		QSignalSpy password(&manager, &FileTransferManager::passwordRequired);
		QSignalSpy updates(&manager, &FileTransferManager::transferUpdated);
		PQFT::FileTransferEngine *engine = manager.m_engine;
		// Finish producing worker signals while the GUI event queue remains
		// undrained. This reproduces a disconnect arriving before the prompts.
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(
			engine,
			[engine]() {
				emit engine->peerBlocked(42);
				emit engine->passwordRequired(QByteArray(16, 'i'));
			},
			Qt::BlockingQueuedConnection));
		manager.disconnectCleanup();
		QVERIFY(QMetaObject::invokeMethod(engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(contact.size(), 0);
		QCOMPARE(blocked.size(), 0);
		QCOMPARE(password.size(), 0);
		QCOMPARE(updates.size(), 0);
		QVERIFY(manager.m_pendingFirstContact.isEmpty());
	}

	void reconnectAcceptsCurrentPromptForReusedSession() {
		FileTransferManager manager;
		QVERIFY(setupTrustFixture(manager));
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		PQFT::FileTransferEngine *engine = manager.m_engine;
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(engine, []() {}, Qt::BlockingQueuedConnection));
		manager.disconnectCleanup();
		manager.refreshPinCache(); // Reconnected user-list events repopulate the cache.
		deliverControl(manager, m_newM1);
		QVERIFY(QMetaObject::invokeMethod(engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
		QCOMPARE(contact.size(), 1);
		const QByteArray newFingerprint = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
		QCOMPARE(contact.at(0).at(1).toByteArray(), newFingerprint);
		QCOMPARE(manager.m_pendingFirstContact.value(42).fingerprint, newFingerprint);
	}

	void shutdownJoinsBusyWorkerBeforeDestroyingEngine() {
		QSemaphore entered;
		std::atomic< bool > completed{ false };
		auto manager                                      = std::make_unique< FileTransferManager >();
		const QPointer< PQFT::FileTransferEngine > engine = manager->m_engine;
		QVERIFY(QMetaObject::invokeMethod(
			engine.data(),
			[&entered, &completed]() {
				QElapsedTimer watchdog;
				watchdog.start();
				entered.release();
				while (!QThread::currentThread()->isInterruptionRequested() && watchdog.elapsed() < 10000) {
					QThread::msleep(1);
				}
				completed.store(true);
			},
			Qt::QueuedConnection));
		QVERIFY(entered.tryAcquire(1, 2000));
		QElapsedTimer shutdown;
		shutdown.start();
		manager.reset();
		QVERIFY(completed.load());
		QVERIFY(engine.isNull());
		QVERIFY2(shutdown.elapsed() < 3000, "Cooperative busy work did not stop before the former unsafe deadline");
	}

private:
	bool setupTrustFixture(FileTransferManager &manager) {
		m_trustDb =
			QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("presented-key-private-fixture"));
		m_trustDb.setDatabaseName(QStringLiteral(":memory:"));
		if (!m_trustDb.open() || !PQFT::FileTransferIdentity::ensureSchema(m_trustDb))
			return false;
		manager.m_trustStore           = std::make_unique< PQFT::PeerTrustStore >(m_trustDb);
		manager.m_identity             = std::make_unique< PQFT::FileTransferIdentity >(m_trustDb);
		manager.m_serverDigestProvider = [this]() { return m_serverDigest; };
		m_self                         = std::make_unique< ClientUser >();
		m_peer                         = std::make_unique< ClientUser >();
		m_self->uiSession              = 41;
		m_peer->uiSession              = 42;
		m_peer->bFileTransferCapable   = true;
		m_peer->bFileTransferDeliveryReceipts = true;
		m_peer->qsName                 = QStringLiteral("Private fixture peer");
		m_channel                      = std::make_unique< Channel >(1234, QStringLiteral("Private fixture channel"));
		m_channel->addUser(m_self.get());
		m_channel->addUser(m_peer.get());
		{
			QWriteLocker lock(&ClientUser::c_qrwlUsers);
			ClientUser::c_qmUsers.insert(41, m_self.get());
			ClientUser::c_qmUsers.insert(42, m_peer.get());
		}
		m_global->uiSession = 41;
		// These manager tests stop at the trust decision, without a usable local signing identity.
		// Refuse M2 explicitly rather than invoking an unset signing callback when acceptance is queued.
		if (!QMetaObject::invokeMethod(
				manager.m_engine,
				[this, &manager]() {
					manager.m_engine->setIdentity({}, [this](QByteArray &, const QByteArray &, const QByteArray &) {
						++m_signCalls;
						return false;
					});
				},
				Qt::BlockingQueuedConnection))
			return false;
		manager.refreshPinCache(); // Match the production user-list event before inbound handshakes.
		return true;
	}
	static QByteArray firstContactFrame() {
		PQFT::SigMLDSA65 signature;
		PQFT::SessionIdentity identity;
		PQFT::SecureBytes secretKey;
		if (!signature.keypair(identity.publicKey, secretKey)) {
			return {};
		}
		PQFT::FileTransferSession session(PQFT::FileTransferSession::Role::Initiator, std::move(identity), {});
		return session.buildM1();
	}

	static void deliverControl(FileTransferManager &manager, const QByteArray &frame) {
		MumbleProto::FileTransferControl message;
		message.set_actor(42);
		message.set_payload(frame.constData(), static_cast< size_t >(frame.size()));
		manager.handleControlMessage(message);
	}

	QByteArray m_oldM1;
	QByteArray m_newM1;
	std::unique_ptr< QTemporaryDir > m_directory;
	std::unique_ptr< Global > m_global;
	QSqlDatabase m_trustDb;
	QByteArray m_serverDigest = QByteArray(20, 's');
	std::atomic< int > m_signCalls{ 0 };
	std::unique_ptr< ClientUser > m_self, m_peer;
	std::unique_ptr< ClientUser > m_replacement;
	std::unique_ptr< Channel > m_channel;
};

QTEST_MAIN(TestFileTransferManager)
#include "TestFileTransferManager.moc"
