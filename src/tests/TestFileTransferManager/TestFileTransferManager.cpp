// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "Global.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

class TestFileTransferManager : public QObject {
	Q_OBJECT
private slots:
	void init() {
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
		m_global.reset();
		Global::g_global_struct = nullptr;
		m_directory.reset();
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
		QCOMPARE(manager.m_pendingFirstContact.value(42).first,
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
		QCOMPARE(manager.m_pendingFirstContact.value(42).first, newFingerprint);
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
};

QTEST_MAIN(TestFileTransferManager)
#include "TestFileTransferManager.moc"
