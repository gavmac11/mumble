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
		m_oldM1                 = firstContactFrame();
		m_newM1                 = firstContactFrame();
		QVERIFY(!m_oldM1.isEmpty());
		QVERIFY(!m_newM1.isEmpty());
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
			42, FileTransferManager::PendingFirstContact{ shownFp, QStringLiteral("shown safety"),
														  QPointer< ClientUser >(m_peer.get()), m_peer->qsName,
														  m_serverDigest });
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
			QVERIFY(manager.m_trustStore->removePin(m_serverDigest, info.peerName));
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
			QVERIFY(manager.m_trustStore->removePin(m_serverDigest, m_peer->qsName));
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

	void receivedFileRechecksPinBeforeSave_data() {
		QTest::addColumn< QString >("change");
		QTest::newRow("verified") << QStringLiteral("verified");
		QTest::newRow("pinned") << QStringLiteral("pinned");
		QTest::newRow("removed") << QStringLiteral("removed");
		QTest::newRow("changed") << QStringLiteral("changed");
		QTest::newRow("storage-unavailable") << QStringLiteral("storage-unavailable");
		QTest::newRow("save-retry") << QStringLiteral("save-retry");
	}
	void receivedFileRechecksPinBeforeSave() {
		QFETCH(QString, change);
		PQFT::SigMLDSA65 sig;
		QByteArray senderPublic;
		PQFT::SecureBytes senderSecret;
		QVERIFY(sig.keypair(senderPublic, senderSecret));
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
		if (change == QLatin1String("removed") || change == QLatin1String("changed")) {
			QVERIFY(receiver.m_trustStore->removePin(m_serverDigest, name));
			if (change == QLatin1String("changed")) {
				const QByteArray other = PQFT::identityFingerprint(PQFT::extractM1IdentityKey(m_newM1));
				QCOMPARE(receiver.m_trustStore->checkAndPin(m_serverDigest, name, other, "replacement"),
						 PQFT::TrustState::Pinned);
			}
		}
		updates.clear();
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
		receiver.saveTransferAs(transfer, target);
		QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
		// Drain actual worker cleanup and GUI forwarding so Aborted cannot hide the rejection.
		QVERIFY(QMetaObject::invokeMethod(receiver.m_engine, []() {}, Qt::BlockingQueuedConnection));
		QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
		const bool expectedSaved = change == QLatin1String("verified") || change == QLatin1String("pinned")
								   || change == QLatin1String("save-retry");
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
		}
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
			manager.m_engine, [&manager]() { manager.m_engine->resolveFirstContact(42, false); },
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
		QSignalSpy contact(&manager, &FileTransferManager::firstContact);
		PQFT::FileTransferEngine *engine = manager.m_engine;
		deliverControl(manager, m_oldM1);
		QVERIFY(QMetaObject::invokeMethod(engine, []() {}, Qt::BlockingQueuedConnection));
		manager.disconnectCleanup();
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
