// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Full engine loopback: two FileTransferEngines wired directly through
// function callbacks (no sockets, no murmur). Covers handshake
// orchestration, manifest exchange, chunk pacing, Merkle completion,
// saving, the password layer and fail-closed behavior.

#include <QTest>

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"
#include "PQFileTransfer/engine/FileTransferEngine.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

// A named namespace (not an anonymous one): the test class below has external
// linkage and GCC's -Wsubobject-linkage rejects members whose type uses an
// anonymous namespace.
namespace PQFTTest {
constexpr unsigned int AliceSession = 1;
constexpr unsigned int BobSession   = 2;

struct TestIdentity {
	QByteArray publicKey;
	PQFT::SecureBytes secretKey;
};

TestIdentity makeIdentity() {
	TestIdentity id;
	PQFT::SigMLDSA65 sig;
	sig.keypair(id.publicKey, id.secretKey);
	return id;
}
} // namespace PQFTTest

using namespace PQFTTest;

class TestFileTransferEngine : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void cleanupTestCase();

	void sendReceiveRoundTrip();
	void passwordRoundTrip();
	void wrongPasswordFailsClosed();
	void duplicateChunkIsFatal();
	void latePasswordSpoolsAllChunks();
	void secondTransferWhileFirstReady();
	void readyTransferDoesNotExpire();
	void firstContactPinFlags();
	void incomingAbortCleansUpSafely();
	void passwordSpoolIsBounded();
	void cancelledPromptTimerCannotRemoveReplacementHandshake_data();
	void cancelledPromptTimerCannotRemoveReplacementHandshake();
	void shutdownRemovesUnsavedPlaintext_data();
	void shutdownRemovesUnsavedPlaintext();
	void unlimitedSendStopsOnWorkerInterruption();

private:
	QString writeTestFile(qsizetype size);
	/// True once `updates` contains `state` for `transferId` ("" matches any).
	static bool sawState(const QSignalSpy &updates, const QByteArray &transferId,
						 PQFT::FTTransferInfo::State state);

	QTemporaryDir m_tempDir;
	TestIdentity m_alice, m_bob;
	QByteArray m_aliceFp, m_bobFp;
};

void TestFileTransferEngine::initTestCase() {
	QVERIFY(m_tempDir.isValid());
	m_alice	= makeIdentity();
	m_bob	= makeIdentity();
	m_aliceFp = PQFT::identityFingerprint(m_alice.publicKey);
	m_bobFp   = PQFT::identityFingerprint(m_bob.publicKey);
}

void TestFileTransferEngine::cleanupTestCase() {
}

void TestFileTransferEngine::cancelledPromptTimerCannotRemoveReplacementHandshake_data() {
	QTest::addColumn< bool >("decline");
	QTest::newRow("disconnect") << false;
	QTest::newRow("declined-prompt") << true;
}

void TestFileTransferEngine::cancelledPromptTimerCannotRemoveReplacementHandshake() {
	QFETCH(bool, decline);
	PQFT::FileTransferEngine engine;
	engine.setIdentity(m_bob.publicKey, {});
	QSignalSpy prompts(&engine, &PQFT::FileTransferEngine::firstContact);
	PQFT::FileTransferSession first(PQFT::FileTransferSession::Role::Initiator, { m_alice.publicKey, {} }, {});
	engine.onControlMessage(AliceSession, first.buildM1());
	QCOMPARE(prompts.size(), 1);
	const QList< QTimer * > timers = engine.findChildren< QTimer * >();
	QCOMPARE(timers.size(), 1);
	const QPointer< QTimer > oldTimer = timers.first();
	QVERIFY(oldTimer->isActive());
	if (decline) {
		engine.resolveFirstContact(AliceSession, false);
	} else {
		engine.abortAll();
	}
	QVERIFY(!oldTimer || !oldTimer->isActive());
	PQFT::FileTransferSession replacement(PQFT::FileTransferSession::Role::Initiator, { m_alice.publicKey, {} }, {});
	const QByteArray m1 = replacement.buildM1();
	engine.onControlMessage(AliceSession, m1);
	QCOMPARE(prompts.size(), 2);
	// Even a timeout already dispatched before cancellation must not erase
	// the replacement handshake with the same peer's session number.
	if (oldTimer) {
		QVERIFY(QMetaObject::invokeMethod(oldTimer.data(), "timeout", Qt::DirectConnection));
	}
	engine.onControlMessage(AliceSession, m1);
	QCOMPARE(prompts.size(), 2);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QVERIFY(oldTimer.isNull());
	engine.abortAll();
}

QString TestFileTransferEngine::writeTestFile(qsizetype size) {
	const QString path = m_tempDir.filePath(QStringLiteral("payload-%1.bin").arg(size));
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return QString();
	const QByteArray data = PQFT::randomBytes(size);
	file.write(data);
	file.close();
	return path;
}

bool TestFileTransferEngine::sawState(const QSignalSpy &updates, const QByteArray &transferId,
									 PQFT::FTTransferInfo::State state) {
	for (const auto &argument : updates) {
		const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
		if (info.state == state && (transferId.isEmpty() || info.transferId == transferId)) {
			return true;
		}
	}
	return false;
}

namespace {
// A directly wired engine pair. Delivery is synchronous (single thread) so
// per-message ordering is exactly preserved, like the TCP path.
struct EnginePair {
	PQFT::FileTransferEngine alice;
	PQFT::FileTransferEngine bob;

	// Interceptor (tid, index, total, data) — called before delivery
	std::function< void(const QByteArray &, quint64, quint64, const QByteArray &) > onChunkDelivered;

	EnginePair(const TestIdentity &aliceId, const TestIdentity &bobId, const QByteArray &aliceFp,
			   const QByteArray &bobFp) {
		auto wireSign = [](const TestIdentity &id) {
			return [&id](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
				PQFT::SigMLDSA65 sigImpl;
				return sigImpl.sign(sig, id.secretKey, msg, ctx);
			};
		};

		alice.setIdentity(aliceId.publicKey, wireSign(aliceId));
		bob.setIdentity(bobId.publicKey, wireSign(bobId));

		// TOFU: each side pins the other
		alice.setPinLookup([bobFp](unsigned int) { return bobFp; });
		bob.setPinLookup([aliceFp](unsigned int) { return aliceFp; });

		alice.setTransport(
			[this](unsigned int target, const QByteArray &payload) {
				Q_UNUSED(target);
				bob.onControlMessage(AliceSession, payload);
			},
			[this](const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) {
				if (onChunkDelivered) {
					onChunkDelivered(transferId, index, total, data);
				}
				bob.onDataMessage(AliceSession, transferId, index, total, data);
			});
		bob.setTransport(
			[this](unsigned int target, const QByteArray &payload) {
				Q_UNUSED(target);
				alice.onControlMessage(BobSession, payload);
			},
			[](const QByteArray &, quint64, quint64, const QByteArray &) {});

	}
};
} // namespace

void TestFileTransferEngine::sendReceiveRoundTrip() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;   // unlimited: drain in one tick
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(600 * 1024 + 17);   // 3 chunks at 256 KiB
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(),
								  { BobSession }),
			 600ull * 1024 + 17);

	// Drive the event loop until Bob reports Ready
	QByteArray readyTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready) {
					readyTransfer = info.transferId;
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(!readyTransfer.isEmpty());

	// Also expect the sender to report completion
	bool senderDone = false;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : aliceUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (!info.incoming && info.state == PQFT::FTTransferInfo::State::Saved) {
					senderDone = true;
					return true;
				}
			}
			return false;
		}(),
		10000);
	QVERIFY(senderDone);

	// Save and compare byte-for-byte
	const QString target = m_tempDir.filePath("received-roundtrip.bin");
	pair.bob.saveTransferAs(readyTransfer, target);
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(target), 10000);

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.size(), received.size());
	QCOMPARE(original.readAll(), received.readAll());
}

void TestFileTransferEngine::unlimitedSendStopsOnWorkerInterruption() {
	const QString source = writeTestFile(8 * 16384);
	QVERIFY(!source.isEmpty());
	int chunks           = 0;
	quint64 startedBytes = 0;
	auto worker          = std::unique_ptr< QThread >(QThread::create([&]() {
		EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
		PQFT::FileTransferEngine::Config fast;
		fast.chunkSize              = 16384;
		fast.sendRateBytesPerSecond = 0;
		pair.alice.setConfig(fast);
		QEventLoop loop;
		QTimer watchdog;
		watchdog.setSingleShot(true);
		QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
		pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &) {
			++chunks;
			QThread::currentThread()->requestInterruption();
			loop.quit();
		};
		startedBytes = pair.alice.startSend(source, "application/octet-stream", false, {}, { BobSession });
		watchdog.start(5000);
		loop.exec();
	}));
	worker->start();
	const bool finished = worker->wait(10000);
	if (!finished) {
		worker->requestInterruption();
		worker->quit();
		worker->wait();
	}
	QVERIFY(finished);
	QCOMPARE(startedBytes, quint64(8 * 16384));
	QCOMPARE(chunks, 1);
}

void TestFileTransferEngine::shutdownRemovesUnsavedPlaintext_data() {
	QTest::addColumn< bool >("saveFirst");
	QTest::newRow("ready-unsaved") << false;
	QTest::newRow("saved-file-preserved") << true;
}

void TestFileTransferEngine::shutdownRemovesUnsavedPlaintext() {
	QFETCH(bool, saveFirst);
	const QString source = writeTestFile(65536);
	QVERIFY(!source.isEmpty());
	const QString target = m_tempDir.filePath("shutdown-saved.bin");
	QString receiveDirectory;
	{
		EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
		PQFT::FileTransferEngine::Config fast;
		fast.sendRateBytesPerSecond = 0;
		pair.alice.setConfig(fast);
		QSignalSpy updates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
		QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { BobSession }) > 0);
		QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Ready), 10000);
		QByteArray transfer;
		for (const auto &arguments : updates) {
			const auto info = arguments.at(0).value< PQFT::FTTransferInfo >();
			if (info.incoming && info.state == PQFT::FTTransferInfo::State::Ready) {
				transfer = info.transferId;
			}
		}
		QVERIFY(!transfer.isEmpty());
		receiveDirectory = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/mumble-ft/"
						   + QString::fromLatin1(transfer.toHex());
		QVERIFY(QFile::exists(receiveDirectory + "/content.bin"));
		if (saveFirst) {
			pair.bob.saveTransferAs(transfer, target);
			QVERIFY(QFile::exists(target));
		}
	}
	const bool retainedPlaintext = QDir(receiveDirectory).exists();
	// Remove only this fixture's residue after observing it, including on a
	// deliberately unfixed build. Never sweep other transfers' directories.
	QDir(receiveDirectory).removeRecursively();
	QVERIFY2(!retainedPlaintext, "Engine shutdown retained the unsaved decrypted receive directory");
	if (saveFirst) {
		QFile original(source), saved(target);
		QVERIFY(original.open(QIODevice::ReadOnly));
		QVERIFY(saved.open(QIODevice::ReadOnly));
		QCOMPARE(saved.readAll(), original.readAll());
	}
}

void TestFileTransferEngine::passwordRoundTrip() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy bobPassword(&pair.bob, &PQFT::FileTransferEngine::passwordRequired);

	const QString source = writeTestFile(300 * 1024);   // 2 chunks
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true,
								  QByteArray("correct horse battery staple"), { BobSession }),
			 300ull * 1024);

	// The receiver parks at WaitingPassword
	QByteArray passwordTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobPassword) {
				passwordTransfer = argument.at(0).toByteArray();
				return true;
			}
			return false;
		}(),
		30000);
	QVERIFY(!passwordTransfer.isEmpty());

	pair.bob.providePassword(passwordTransfer, QByteArray("correct horse battery staple"));

	QByteArray readyTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.transferId == passwordTransfer
					&& info.state == PQFT::FTTransferInfo::State::Ready) {
					readyTransfer = info.transferId;
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(!readyTransfer.isEmpty());

	const QString target = m_tempDir.filePath("received-password.bin");
	pair.bob.saveTransferAs(readyTransfer, target);
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(target), 10000);

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.size(), received.size());
	QCOMPARE(original.readAll(), received.readAll());
}

void TestFileTransferEngine::wrongPasswordFailsClosed() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy bobPassword(&pair.bob, &PQFT::FileTransferEngine::passwordRequired);

	const QString source = writeTestFile(64 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true, QByteArray("the right password"),
								  { BobSession }),
			 64ull * 1024);

	QByteArray passwordTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobPassword) {
				passwordTransfer = argument.at(0).toByteArray();
				return true;
			}
			return false;
		}(),
		30000);

	pair.bob.providePassword(passwordTransfer, QByteArray("the wrong password"));

	// Wrong password -> Failed with the one generic error, no partial output
	bool failedWithGenericError = false;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.transferId == passwordTransfer
					&& info.state == PQFT::FTTransferInfo::State::Failed) {
					failedWithGenericError = info.error.contains("Decryption failed");
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(failedWithGenericError);

	// No temp directory survived for that transfer
	const QString dir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(passwordTransfer.toHex()));
	QVERIFY(!QFile::exists(dir + "/content.bin"));
}

void TestFileTransferEngine::duplicateChunkIsFatal() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(32 * 1024);   // two 16 KiB chunks
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { BobSession }),
			 32ull * 1024);

	// Redeliver chunk 0 while the transfer is in flight: a misbehaving relay
	// duplicating a frame must kill the receive job (§9). The duplicate is
	// delivered first, so the genuine frame becomes the duplicate index.
	pair.onChunkDelivered = [&pair](const QByteArray &transferId, quint64 index, quint64 total,
									const QByteArray &data) {
		static bool replayed = false;
		if (!replayed && index == 0) {
			replayed = true;
			pair.bob.onDataMessage(AliceSession, transferId, index, total, data);
		}
	};

	bool failedClosed = false;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Failed) {
					failedClosed = info.error.contains("Decryption failed");
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(failedClosed);

	// No partial output remains for the failed transfer (identified from the
	// failure update)
	QByteArray failedTransfer;
	for (const auto &argument : bobUpdates) {
		const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Failed) {
			failedTransfer = info.transferId;
		}
	}
	QVERIFY(!failedTransfer.isEmpty());
	const QString dir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(failedTransfer.toHex()));
	QVERIFY(!QFile::exists(dir + "/content.bin"));
}

// Review regression: the sender streams without a readiness acknowledgement,
// so a receiver waiting for the password must keep EVERY chunk. The old
// 8-entry memory buffer silently discarded the rest and the transfer could
// never complete.
void TestFileTransferEngine::latePasswordSpoolsAllChunks() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy bobPassword(&pair.bob, &PQFT::FileTransferEngine::passwordRequired);
	QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

	// 13 chunks — deliberately past the old 8-entry buffer
	const QString source = writeTestFile(200 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true,
								  QByteArray("a late password"), { BobSession }),
			 200ull * 1024);

	// Let the sender run to completion BEFORE any password is provided.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(aliceUpdates, QByteArray(),
									 PQFT::FTTransferInfo::State::Saved),
							 30000);
	QVERIFY(sawState(bobUpdates, QByteArray(), PQFT::FTTransferInfo::State::WaitingPassword));

	QByteArray passwordTransfer;
	for (const auto &argument : bobPassword) {
		passwordTransfer = argument.at(0).toByteArray();
	}
	QVERIFY(!passwordTransfer.isEmpty());

	pair.bob.providePassword(passwordTransfer, QByteArray("a late password"));

	// Every spooled chunk replays and the transfer completes.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, passwordTransfer,
									 PQFT::FTTransferInfo::State::Ready),
							 30000);

	const QString target = m_tempDir.filePath("received-late-password.bin");
	pair.bob.saveTransferAs(passwordTransfer, target);
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(target), 10000);

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.size(), received.size());
	QCOMPARE(original.readAll(), received.readAll());
}

// Review regression: a new M1 from the same sender used to be routed into
// the established session of an earlier transfer, killing it while the new
// handshake timed out.
void TestFileTransferEngine::secondTransferWhileFirstReady() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);

	const QString first = writeTestFile(64 * 1024);
	QVERIFY(!first.isEmpty());
	QCOMPARE(pair.alice.startSend(first, "application/octet-stream", false, QByteArray(),
								  { BobSession }),
			 64ull * 1024);

	QByteArray firstTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready) {
					firstTransfer = info.transferId;
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(!firstTransfer.isEmpty());
	// Deliberately left unsaved: the first transfer stays Ready.

	const QString second = writeTestFile(100 * 1024);
	QVERIFY(!second.isEmpty());
	QCOMPARE(pair.alice.startSend(second, "application/octet-stream", false, QByteArray(),
								  { BobSession }),
			 100ull * 1024);

	QByteArray secondTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready
					&& info.transferId != firstTransfer) {
					secondTransfer = info.transferId;
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(!secondTransfer.isEmpty());

	// The first transfer was untouched by the second one's handshake.
	QVERIFY(!sawState(bobUpdates, firstTransfer, PQFT::FTTransferInfo::State::Failed));
	const QString firstDir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(firstTransfer.toHex()));
	QVERIFY(QFile::exists(firstDir + "/content.bin"));

	// ...and both saved copies match their sources.
	const QString firstTarget  = m_tempDir.filePath("received-first.bin");
	const QString secondTarget = m_tempDir.filePath("received-second.bin");
	pair.bob.saveTransferAs(firstTransfer, firstTarget);
	pair.bob.saveTransferAs(secondTransfer, secondTarget);
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(firstTarget) && QFile::exists(secondTarget), 10000);

	QFile originalFirst(first), originalSecond(second);
	QFile receivedFirst(firstTarget), receivedSecond(secondTarget);
	QVERIFY(originalFirst.open(QIODevice::ReadOnly) && receivedFirst.open(QIODevice::ReadOnly));
	QVERIFY(originalSecond.open(QIODevice::ReadOnly) && receivedSecond.open(QIODevice::ReadOnly));
	QCOMPARE(originalFirst.readAll(), receivedFirst.readAll());
	QCOMPARE(originalSecond.readAll(), receivedSecond.readAll());
}

// Review regression: the idle timer used to keep running after Ready and,
// 60 s later, marked the verified transfer Failed and deleted its temp file.
void TestFileTransferEngine::readyTransferDoesNotExpire() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond   = 0;
	fast.receiveIdleTimeoutMSecs  = 500;   // would have killed it quickly
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(64 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(),
								  { BobSession }),
			 64ull * 1024);

	QByteArray readyTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready) {
					readyTransfer = info.transferId;
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(!readyTransfer.isEmpty());

	// Sit well past the idle timeout with the transfer Ready.
	QTest::qWait(2000);
	QVERIFY(!sawState(bobUpdates, readyTransfer, PQFT::FTTransferInfo::State::Failed));
	const QString dir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(readyTransfer.toHex()));
	QVERIFY(QFile::exists(dir + "/content.bin"));

	const QString target = m_tempDir.filePath("received-no-expiry.bin");
	pair.bob.saveTransferAs(readyTransfer, target);
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(target), 10000);

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.readAll(), received.readAll());
}

// Review regression: only the SENDER side (post-M4, key authenticated by the
// handshake) may pin on first observation; a receiver-side plain M1 is an
// unauthenticated claim and declining it must leave nothing behind.
void TestFileTransferEngine::firstContactPinFlags() {
	{
		// Receiver side: unknown peer parks the handshake and reports
		// pinOnObservation = false; declining drops it without an answer.
		EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
		PQFT::FileTransferEngine::Config config;
		config.sendRateBytesPerSecond = 0;
		config.handshakeTimeoutMSecs = 500;
		pair.alice.setConfig(config);
		pair.bob.setConfig(config);
		pair.bob.setPinLookup([](unsigned int) { return QByteArray(); });   // first contact

		QSignalSpy bobFirstContact(&pair.bob, &PQFT::FileTransferEngine::firstContact);
		QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

		const QString source = writeTestFile(64 * 1024);
		QVERIFY(!source.isEmpty());
		QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(),
									  { BobSession }),
				 64ull * 1024);

		QTRY_VERIFY_WITH_TIMEOUT(bobFirstContact.count() >= 1, 10000);
		QCOMPARE(bobFirstContact.first().at(4).toBool(), false);

		pair.bob.resolveFirstContact(AliceSession, false);   // declined

		// No M2 was ever sent: the initiator fails once its handshake
		// timeout expires instead of hanging.
		QTRY_VERIFY_WITH_TIMEOUT(sawState(aliceUpdates, QByteArray(),
										 PQFT::FTTransferInfo::State::Failed),
								 10000);
	}
	{
		// Sender side: first use after a completed (authenticated) handshake
		// reports pinOnObservation = true and the transfer proceeds.
		EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
		PQFT::FileTransferEngine::Config fast;
		fast.sendRateBytesPerSecond = 0;
		pair.alice.setConfig(fast);
		pair.bob.setConfig(fast);
		pair.alice.setPinLookup([](unsigned int) { return QByteArray(); });   // first use

		QSignalSpy aliceFirstContact(&pair.alice, &PQFT::FileTransferEngine::firstContact);
		QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);

		const QString source = writeTestFile(64 * 1024);
		QVERIFY(!source.isEmpty());
		QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(),
									  { BobSession }),
				 64ull * 1024);

		QTRY_VERIFY_WITH_TIMEOUT(aliceFirstContact.count() >= 1, 30000);
		QCOMPARE(aliceFirstContact.first().at(4).toBool(), true);
		QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, QByteArray(),
										 PQFT::FTTransferInfo::State::Ready),
								 30000);
	}
}

// Review regression: an incoming ABORT travels onControlMessage ->
// processControlForReceive -> cleanupReceive, where the job pointer handed
// down used to be a reference to the map's own shared_ptr — erasing the map
// entry destroyed the job mid-cleanup (use-after-free on job.spool).
void TestFileTransferEngine::incomingAbortCleansUpSafely() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config trickled;
	trickled.sendRateBytesPerSecond = 32 * 1024;   // one 16 KiB chunk per tick
	trickled.chunkSize              = 16 * 1024;
	pair.alice.setConfig(trickled);
	pair.bob.setConfig(trickled);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(200 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(),
								  { BobSession }),
			 200ull * 1024);

	// Wait until the transfer is live (manifest verified, chunks trickling),
	// then abort from the sender side.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, QByteArray(),
									 PQFT::FTTransferInfo::State::Transferring),
							 30000);
	pair.alice.abortTransfer(QByteArray());   // invalid id: must not disturb anything

	QByteArray transferId;
	for (const auto &argument : bobUpdates) {
		const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Transferring) {
			transferId = info.transferId;
		}
	}
	QVERIFY(!transferId.isEmpty());
	pair.alice.abortTransfer(transferId);

	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, transferId,
									 PQFT::FTTransferInfo::State::Aborted),
							 10000);
	QVERIFY(sawState(aliceUpdates, transferId, PQFT::FTTransferInfo::State::Aborted));

	// Partial output is gone.
	const QString dir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(transferId.toHex()));
	QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir + "/content.bin"), 10000);
}

// Review regression: while the password prompt is up, records were spooled
// before any validation — repeating one chunk grew the spool past the
// manifest's size (a 200 KB transfer reached 1.8 MB) and kept refreshing the
// idle timer. Index, length and duplicates are now checked before writing,
// so the flood fails the transfer instead.
void TestFileTransferEngine::passwordSpoolIsBounded() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy bobPassword(&pair.bob, &PQFT::FileTransferEngine::passwordRequired);

	const QString source = writeTestFile(200 * 1024);   // 204,800 bytes: 13 chunks
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true,
								  QByteArray("some password"), { BobSession }),
			 200ull * 1024);

	QByteArray passwordTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobPassword) {
				passwordTransfer = argument.at(0).toByteArray();
				return true;
			}
			return false;
		}(),
		30000);
	QVERIFY(!passwordTransfer.isEmpty());

	// The reviewer's flood: 100 redeliveries of chunk 0 while the password
	// prompt is up. The first repeat must be fatal; nothing may be spooled
	// beyond the single legitimate copy.
	pair.onChunkDelivered = [&pair](const QByteArray &transferId, quint64 index, quint64 total,
									const QByteArray &data) {
		if (index != 0) {
			return;
		}
		static bool flooded = false;
		if (!flooded) {
			flooded = true;
			for (int i = 0; i < 100; ++i) {
				pair.bob.onDataMessage(AliceSession, transferId, index, total, data);
			}
		}
	};

	bool failedWithGenericError = false;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.transferId == passwordTransfer
					&& info.state == PQFT::FTTransferInfo::State::Failed) {
					failedWithGenericError = info.error.contains("Decryption failed");
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(failedWithGenericError);

	const QString dir =
		QDir::temp().absoluteFilePath("mumble-ft/" + QString::fromLatin1(passwordTransfer.toHex()));
	QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir + "/content.bin")
								 && !QFile::exists(dir + "/chunks.spool"),
							 10000);
}

QTEST_MAIN(TestFileTransferEngine)
#include "TestFileTransferEngine.moc"
