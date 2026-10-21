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
#include "PQFileTransfer/engine/FTMessages.h"
#include "PQFileTransfer/engine/FileTransferEngine.h"
#include "PQFileTransfer/engine/FileTransferSession.h"
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <limits>
#ifndef Q_OS_WIN
#	include <sys/stat.h>
#endif

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
	void relayRetriesKeepCiphertextAndRequireAdmission();
	void synchronousRelayAdmissionCompletes();
	void incompleteRecipientHandshakeSendsNoContent();
	void missingReceiptReportsPartialDelivery();
	void peerResetKeepsUnrelatedReadyFileAndRetiresPrompt();
	void initTestCase();

	void privateReceiveStorage_data();
	void privateReceiveStorage();
	void predictableReceivePathCannotRedirectWrites();
	void earlyChunksKeepTheirTransferIdentity();
	void unrelatedChunksDoNotExtendPendingReceive();
	void sendReceiveRoundTrip();
	void incomingIdentitySurvivesPeerRename();
	void selectedPinSurvivesCacheRemoval();
	void passwordRoundTrip();
	void wrongPasswordFailsClosed();
	void duplicateChunkIsFatal();
	void latePasswordSpoolsAllChunks();
	void secondTransferWhileFirstReady();
	void readyTransferDoesNotExpire();
	void firstContactPinFlags();
	void delayedIdentityVerificationStillDelivers();
	void unansweredHandshakeStillExpires();
	void verificationTimeoutIsNormalized_data();
	void verificationTimeoutIsNormalized();
	void quickRecipientDoesNotShortenOtherVerification();
	void incomingAbortCleansUpSafely();
	void passwordSpoolIsBounded();
	void cancelledPromptTimerCannotRemoveReplacementHandshake_data();
	void cancelledPromptTimerCannotRemoveReplacementHandshake();
	void shutdownRemovesUnsavedPlaintext_data();
	void shutdownRemovesUnsavedPlaintext();
	void unlimitedSendStopsOnWorkerInterruption();
	void sendingRespectsConfiguredRate_data();
	void sendingRespectsConfiguredRate();
	void simultaneousSendsShareRateLimit();
	void unlimitedSendYieldsToQueuedAbort();
	void synchronousChunkAbortStaysAborted();
	void abortingAnotherJobDoesNotStopPacing();
	void synchronousCompletionAbortStaysAborted();
	void pausedDeliveryDoesNotReleaseUnboundedCredit();
	void slowSendMakesProgress_data();
	void slowSendMakesProgress();
	void frequentRefillsPreserveElapsedCredit();
	void saveDoesNotDeleteUnrelatedPart();
	void confirmedSaveReplacesAtomically();
	void failedSavePreservesReceiveForRetry();
	void savedPermissionsMatchNormalCreation_data();
	void savedPermissionsMatchNormalCreation();
	void receiveFailureStopsSenderImmediately();
	void saveSyncFailureIsRetryable_data();
	void saveSyncFailureIsRetryable();

private:
	QString writeTestFile(qsizetype size);
	/// True once `updates` contains `state` for `transferId` ("" matches any).
	static bool sawState(const QSignalSpy &updates, const QByteArray &transferId, PQFT::FTTransferInfo::State state);

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
	QList< QTimer * > timers;
	for (QTimer *timer : engine.findChildren< QTimer * >()) {
		if (timer->isActive())
			timers.append(timer);
	}
	QCOMPARE(timers.size(), 1);
	const QPointer< QTimer > oldTimer = timers.first();
	QVERIFY(oldTimer->isActive());
	if (decline) {
		engine.resolveFirstContact(AliceSession, false, prompts.first().at(5).toULongLong());
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

void TestFileTransferEngine::relayRetriesKeepCiphertextAndRequireAdmission() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 64 * 1024;
	pair.alice.setConfig(config);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QByteArray firstCiphertext;
	QByteArray transfer;
	int attempts           = 0;
	bool ciphertextChanged = false;
	pair.alice.setTargetedChunkTransport(
		[&](const QList< unsigned int > &targets, const QByteArray &id, quint64 index, quint64 total,
			const QByteArray &ciphertext) {
			transfer = id;
			if (firstCiphertext.isEmpty())
				firstCiphertext = ciphertext;
			ciphertextChanged = ciphertextChanged || firstCiphertext != ciphertext || index != 0
								|| targets != QList< unsigned int >{ BobSession };
			++attempts;
			if (attempts == 1)
				return false; // local queue backpressure
			QTimer::singleShot(0, &pair.alice, [&, id, index, total, ciphertext]() {
				if (attempts == 2) {
					pair.alice.relayResponse(id, index, false, 100, QStringLiteral("aggregate busy"));
				} else {
					pair.bob.onDataMessage(AliceSession, id, index, total, ciphertext);
					pair.alice.relayResponse(id, index + 1, true, 0, {}); // unrelated acknowledgement
					pair.alice.relayResponse(id, index, true, 0, {});
				}
			});
			return true;
		},
		true);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(attempts >= 2, 5000);
	QVERIFY(!sawState(sent, transfer, PQFT::FTTransferInfo::State::Saved));
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, transfer, PQFT::FTTransferInfo::State::Saved), 10000);
	QVERIFY(sawState(received, transfer, PQFT::FTTransferInfo::State::Ready));
	QCOMPARE(attempts, 3);
	QVERIFY(!ciphertextChanged);
}

void TestFileTransferEngine::synchronousRelayAdmissionCompletes() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 64 * 1024;
	pair.alice.setConfig(config);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QByteArray transfer;
	int chunks = 0;
	pair.alice.setTargetedChunkTransport(
		[&](const QList< unsigned int > &, const QByteArray &id, quint64 index, quint64 total,
			const QByteArray &ciphertext) {
			transfer = id;
			++chunks;
			pair.bob.onDataMessage(AliceSession, id, index, total, ciphertext);
			pair.alice.relayResponse(id, index, true, 0, {});
			return true;
		},
		true);
	QVERIFY(pair.alice.startSend(writeTestFile(192 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, transfer, PQFT::FTTransferInfo::State::Saved), 5000);
	QVERIFY(sawState(received, transfer, PQFT::FTTransferInfo::State::Ready));
	QCOMPARE(chunks, 3);
}

void TestFileTransferEngine::incompleteRecipientHandshakeSendsNoContent() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.handshakeTimeoutMSecs            = 100;
	config.identityVerificationTimeoutMSecs = 100;
	pair.alice.setConfig(config);
	int chunks = 0;
	pair.alice.setTransport(
		[&](unsigned int target, const QByteArray &frame) {
			if (target == BobSession)
				pair.bob.onControlMessage(AliceSession, frame);
		},
		[&](const QByteArray &, quint64, quint64, const QByteArray &) { ++chunks; });
	QSignalSpy updates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp }, { 3, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Failed), 5000);
	QCOMPARE(chunks, 0);
	QVERIFY(!sawState(updates, {}, PQFT::FTTransferInfo::State::Saved));
}

void TestFileTransferEngine::missingReceiptReportsPartialDelivery() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine third;
	third.setIdentity(m_bob.publicKey, [&](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
		PQFT::SigMLDSA65 signer;
		return signer.sign(sig, m_bob.secretKey, msg, ctx);
	});
	third.setPinLookup([&](unsigned int) { return m_aliceFp; });
	third.setTransport(
		[&](unsigned int, const QByteArray &frame) {
			if ((static_cast< unsigned char >(frame[0]) & 15) != PQFT::FTFrame::TypeReceipt)
				pair.alice.onControlMessage(3, frame);
		},
		{});
	pair.alice.setTransport(
		[&](unsigned int target, const QByteArray &frame) {
			(target == BobSession ? pair.bob : third).onControlMessage(AliceSession, frame);
		},
		[&](const QByteArray &id, quint64 index, quint64 total, const QByteArray &ciphertext) {
			pair.bob.onDataMessage(AliceSession, id, index, total, ciphertext);
			third.onDataMessage(AliceSession, id, index, total, ciphertext);
		});
	PQFT::FileTransferEngine::Config config;
	config.receiveIdleTimeoutMSecs = 1000;
	pair.alice.setConfig(config);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp }, { 3, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, {}, PQFT::FTTransferInfo::State::PartiallyDelivered), 10000);
	QVERIFY(!sawState(sent, {}, PQFT::FTTransferInfo::State::Saved));
	const auto final = sent.last().first().value< PQFT::FTTransferInfo >();
	QCOMPARE(final.deliveredRecipients, QList< unsigned int >{ BobSession });
	QCOMPARE(final.failedRecipients, QList< unsigned int >{ 3 });
}

void TestFileTransferEngine::peerResetKeepsUnrelatedReadyFileAndRetiresPrompt() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 5000);
	const QByteArray id = received.last().first().value< PQFT::FTTransferInfo >().transferId;
	QVERIFY(pair.bob.m_receiveJobs.contains(id));
	const auto ready        = pair.bob.m_receiveJobs.value(id);
	const QString directory = ready->tempDirectory->path();
	QVERIFY(QDir(directory).exists());
	pair.bob.setPinLookup([&](unsigned int session) { return session == AliceSession ? m_aliceFp : QByteArray(); });
	QSignalSpy prompts(&pair.bob, &PQFT::FileTransferEngine::firstContact);
	QSignalSpy retired(&pair.bob, &PQFT::FileTransferEngine::firstContactRetired);
	PQFT::FileTransferSession pending(PQFT::FileTransferSession::Role::Initiator, { m_alice.publicKey, {} }, {});
	pair.bob.onControlMessage(3, pending.buildM1());
	QCOMPARE(prompts.count(), 1);
	pair.bob.abortPeer(3);
	QCOMPARE(retired.count(), 1);
	QCOMPARE(retired.first().at(1).toULongLong(), prompts.first().at(5).toULongLong());
	QVERIFY(pair.bob.m_receiveJobs.contains(id));
	QCOMPARE(ready->lastState, PQFT::FTTransferInfo::State::Ready);
	QVERIFY(QDir(directory).exists());
}

void TestFileTransferEngine::sendingRespectsConfiguredRate_data() {
	QTest::addColumn< quint32 >("rate");
	QTest::addColumn< quint32 >("chunkSize");
	QTest::addColumn< int >("fileSize");
	QTest::newRow("default") << quint32(4 * 1024 * 1024) << quint32(256 * 1024) << 1024 * 1024;
	QTest::newRow("below-one-chunk-per-tick") << quint32(1024 * 1024) << quint32(256 * 1024) << 512 * 1024;
	QTest::newRow("slow") << quint32(32 * 1024) << quint32(16 * 1024) << 64 * 1024;
}

void TestFileTransferEngine::sendingRespectsConfiguredRate() {
	QFETCH(quint32, rate);
	QFETCH(quint32, chunkSize);
	QFETCH(int, fileSize);
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = rate;
	config.chunkSize              = chunkSize;
	pair.alice.setConfig(config);
	QElapsedTimer elapsed;
	quint64 sentBytes = 0;
	QString violation;
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &data) {
		sentBytes += static_cast< quint64 >(data.size());
		// Allow only clock quantization, never a free chunk or per-tick overshoot.
		const quint64 allowed = static_cast< quint64 >(rate) * static_cast< quint64 >(elapsed.elapsed() + 2) / 1000;
		if (sentBytes > allowed && violation.isEmpty())
			violation = QString("Sent %1 bytes in %2 ms at configured %3 bytes/s")
							.arg(sentBytes)
							.arg(elapsed.elapsed())
							.arg(rate);
	};
	QSignalSpy updates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(fileSize);
	elapsed.start();
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Ready), 15000);
	QVERIFY2(violation.isEmpty(), qPrintable(violation));
}

void TestFileTransferEngine::slowSendMakesProgress_data() {
	QTest::addColumn< quint32 >("rate");
	QTest::addColumn< quint32 >("chunkSize");
	QTest::addColumn< bool >("refreshConfig");
	QTest::newRow("unchanged-config-refresh") << quint32(32 * 1024) << quint32(16 * 1024) << true;
	QTest::newRow("large-chunk-at-low-rate") << quint32(64 * 1024) << quint32(1024 * 1024) << false;
}

void TestFileTransferEngine::slowSendMakesProgress() {
	QFETCH(quint32, rate);
	QFETCH(quint32, chunkSize);
	QFETCH(bool, refreshConfig);
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond  = rate;
	config.chunkSize               = chunkSize;
	config.receiveIdleTimeoutMSecs = 700;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);
	QTimer refresh;
	connect(&refresh, &QTimer::timeout, &pair.alice, [&]() { pair.alice.setConfig(config); });
	if (refreshConfig)
		refresh.start(80);
	int chunks             = 0;
	qsizetype largestChunk = 0;
	pair.onChunkDelivered  = [&](const QByteArray &, quint64, quint64, const QByteArray &data) {
		++chunks;
		largestChunk = qMax(largestChunk, data.size());
	};
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(80 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready)
								 || sawState(received, {}, PQFT::FTTransferInfo::State::Failed),
							 5000);
	QVERIFY(sawState(received, {}, PQFT::FTTransferInfo::State::Ready));
	QCOMPARE(chunks, 5);
	QVERIFY(largestChunk <= 16 * 1024 + PQFT::TagSize);
	QByteArray id;
	for (const auto &args : received) {
		const auto info = args.first().value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Ready)
			id = info.transferId;
	}
	const QString target = m_tempDir.filePath(QString::fromLatin1(id.toHex()) + ".bin");
	pair.bob.saveTransferAs(id, target);
	QFile output(target);
	QFile input(source);
	QVERIFY(output.open(QIODevice::ReadOnly));
	QVERIFY(input.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), input.readAll());
}

void TestFileTransferEngine::frequentRefillsPreserveElapsedCredit() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 64 * 1024;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QPointer< QTimer > pacer;
	for (QTimer *timer : pair.alice.findChildren< QTimer * >()) {
		if (timer->isActive() && timer->interval() == 50)
			pacer = timer;
	}
	QVERIFY(pacer);
	// Request refills far more often than once per millisecond. The same
	// elapsed budget must survive the calls, instead of rounding each to zero.
	QElapsedTimer elapsed;
	elapsed.start();
	while (elapsed.elapsed() < 2000 && !sawState(received, {}, PQFT::FTTransferInfo::State::Ready)) {
		QVERIFY(QMetaObject::invokeMethod(pacer.data(), "timeout", Qt::DirectConnection));
	}
	QVERIFY(sawState(received, {}, PQFT::FTTransferInfo::State::Ready));
}

void TestFileTransferEngine::receiveFailureStopsSenderImmediately() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.chunkSize              = 16 * 1024;
	config.sendRateBytesPerSecond = 64 * 1024;
	pair.alice.setConfig(config);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QByteArray transfer;
	int chunks            = 0;
	pair.onChunkDelivered = [&](const QByteArray &id, quint64, quint64, const QByteArray &) {
		transfer = id;
		++chunks;
	};
	QVERIFY(pair.alice.startSend(writeTestFile(1024 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } }));
	QTRY_VERIFY_WITH_TIMEOUT(chunks > 0, 5000);
	const int before = chunks;
	pair.bob.transportFailed(AliceSession, transfer, "receive queue stalled");
	QVERIFY(sawState(sent, transfer, PQFT::FTTransferInfo::State::Failed));
	QVERIFY(sawState(received, transfer, PQFT::FTTransferInfo::State::Failed));
	QTest::qWait(300);
	QCOMPARE(chunks, before);
}

void TestFileTransferEngine::savedPermissionsMatchNormalCreation_data() {
	QTest::addColumn< bool >("replace");
	QTest::addColumn< int >("creationMask");
	QTest::newRow("new-default") << false << 0022;
	QTest::newRow("replace-default") << true << 0022;
	QTest::newRow("new-private") << false << 0077;
	QTest::newRow("replace-private") << true << 0077;
}

void TestFileTransferEngine::savedPermissionsMatchNormalCreation() {
	QFETCH(bool, replace);
	QFETCH(int, creationMask);
#ifndef Q_OS_WIN
	const auto previousMask = ::umask(static_cast< mode_t >(creationMask));
	const auto restoreMask  = qScopeGuard([&]() { ::umask(previousMask); });
#else
	Q_UNUSED(creationMask);
#endif
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(64 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }));
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 5000);
	const auto ready = received.last().first().value< PQFT::FTTransferInfo >();
	QFile ordinary(m_tempDir.filePath(replace ? QString("ordinary-replace-%1.bin").arg(creationMask)
											  : QString("ordinary-new-%1.bin").arg(creationMask)));
	QVERIFY(ordinary.open(QIODevice::WriteOnly | QIODevice::NewOnly));
	const auto expected = ordinary.permissions();
	ordinary.close();
	const QString target = m_tempDir.filePath(replace ? QString("permissions-replace-%1.bin").arg(creationMask)
													  : QString("permissions-new-%1.bin").arg(creationMask));
	if (replace) {
		QFile existing(target);
		QVERIFY(existing.open(QIODevice::WriteOnly));
	}
	pair.bob.saveTransferAs(ready.transferId, target, replace);
	QVERIFY(sawState(received, ready.transferId, PQFT::FTTransferInfo::State::Saved));
	QCOMPARE(QFile::permissions(target), expected);
	QCOMPARE(QDir(m_tempDir.path())
				 .entryList({ ".mumble-ft-mode-*", ".mumble-ft-save-*" }, QDir::Files | QDir::Hidden)
				 .size(),
			 0);
}

void TestFileTransferEngine::saveDoesNotDeleteUnrelatedPart() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(64 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	QByteArray id;
	for (const auto &args : received) {
		const auto info = args.first().value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Ready)
			id = info.transferId;
	}
	const QString target = m_tempDir.filePath("existing-target.bin");
	QFile original(target);
	QVERIFY(original.open(QIODevice::WriteOnly));
	QCOMPARE(original.write("original target"), qint64(15));
	original.close();
	QFile unrelated(target + ".part");
	QVERIFY(unrelated.open(QIODevice::WriteOnly));
	QCOMPARE(unrelated.write("unrelated part"), qint64(14));
	unrelated.close();
	const qsizetype before = received.size();
	pair.bob.saveTransferAs(id, target);
	QVERIFY(received.size() > before);
	QCOMPARE(received.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Ready);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QCOMPARE(original.readAll(), QByteArray("original target"));
	QVERIFY(unrelated.open(QIODevice::ReadOnly));
	QCOMPARE(unrelated.readAll(), QByteArray("unrelated part"));
	const QString retry = m_tempDir.filePath("retry-target.bin");
	pair.bob.saveTransferAs(id, retry);
	QCOMPARE(received.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
	QFile output(retry);
	QFile input(source);
	QVERIFY(output.open(QIODevice::ReadOnly));
	QVERIFY(input.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), input.readAll());
}

void TestFileTransferEngine::confirmedSaveReplacesAtomically() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(80 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	const QByteArray id  = received.last().first().value< PQFT::FTTransferInfo >().transferId;
	const QString target = m_tempDir.filePath("confirmed-target.bin");
	QFile original(target);
	QVERIFY(original.open(QIODevice::WriteOnly));
	QCOMPARE(original.write("original target"), qint64(15));
	original.close();
	pair.bob.saveTransferAs(id, target, true);
	QCOMPARE(received.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
	QFile output(target);
	QFile input(source);
	QVERIFY(output.open(QIODevice::ReadOnly));
	QVERIFY(input.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), input.readAll());
	QCOMPARE(QDir(m_tempDir.path()).entryList({ ".mumble-ft-save-*" }, QDir::Files | QDir::Hidden).size(), 0);
}

void TestFileTransferEngine::failedSavePreservesReceiveForRetry() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(96 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	const QByteArray id     = received.last().first().value< PQFT::FTTransferInfo >().transferId;
	const QString directory = m_tempDir.filePath("non-file-target");
	QVERIFY(QDir().mkpath(directory));
	QFile sentinel(QDir(directory).filePath("sentinel"));
	QVERIFY(sentinel.open(QIODevice::WriteOnly));
	QCOMPARE(sentinel.write("preserved"), qint64(9));
	sentinel.close();
	pair.bob.saveTransferAs(id, directory, true);
	const auto failed = received.last().first().value< PQFT::FTTransferInfo >();
	QCOMPARE(failed.state, PQFT::FTTransferInfo::State::Ready);
	QVERIFY(!failed.error.isEmpty());
	QVERIFY(sentinel.open(QIODevice::ReadOnly));
	QCOMPARE(sentinel.readAll(), QByteArray("preserved"));
	const QString target = m_tempDir.filePath("save-retry.bin");
	pair.bob.saveTransferAs(id, target);
	QCOMPARE(received.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
	QFile output(target);
	QFile input(source);
	QVERIFY(output.open(QIODevice::ReadOnly));
	QVERIFY(input.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), input.readAll());
	QCOMPARE(QDir(m_tempDir.path()).entryList({ ".mumble-ft-save-*" }, QDir::Files | QDir::Hidden).size(), 0);
}

void TestFileTransferEngine::saveSyncFailureIsRetryable_data() {
	QTest::addColumn< bool >("replace");
	QTest::addColumn< bool >("afterPublication");
	QTest::newRow("new-file-before-publication") << false << false;
	QTest::newRow("replacement-before-publication") << true << false;
	QTest::newRow("new-file-after-publication") << false << true;
	QTest::newRow("replacement-after-publication") << true << true;
}

void TestFileTransferEngine::saveSyncFailureIsRetryable() {
	QFETCH(bool, replace);
	QFETCH(bool, afterPublication);
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(80 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	const QByteArray id  = received.last().first().value< PQFT::FTTransferInfo >().transferId;
	const QString target = m_tempDir.filePath("sync-failure-" + QString::fromUtf8(QTest::currentDataTag()) + ".bin");
	if (replace) {
		QFile old(target);
		QVERIFY(old.open(QIODevice::WriteOnly));
		QCOMPARE(old.write("original"), qint64(8));
	}
	const QStringList before = QDir(m_tempDir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
	const auto fileSync      = pair.bob.m_syncSaveFile;
	const auto publicationSync = pair.bob.m_syncSavePublication;
	bool reachedWrittenStage   = false;
	if (afterPublication) {
		pair.bob.m_syncSavePublication = [](const QString &) { return false; };
	} else {
		pair.bob.m_syncSaveFile = [&](QFileDevice &stage) {
			reachedWrittenStage = stage.pos() == 80 * 1024;
			return false;
		};
	}
	pair.bob.saveTransferAs(id, target, replace);
	const auto failed = received.last().first().value< PQFT::FTTransferInfo >();
	QCOMPARE(failed.state, PQFT::FTTransferInfo::State::Ready);
	QVERIFY(!failed.error.isEmpty());
	QVERIFY(!sawState(received, id, PQFT::FTTransferInfo::State::Saved));
	QFile input(source);
	QVERIFY(input.open(QIODevice::ReadOnly));
	const QByteArray expected = input.readAll();
	if (afterPublication || replace) {
		QFile output(target);
		QVERIFY(output.open(QIODevice::ReadOnly));
		QCOMPARE(output.readAll(), afterPublication ? expected : QByteArray("original"));
	} else {
		QVERIFY(!QFile::exists(target));
	}
	if (!afterPublication) {
		QVERIFY(reachedWrittenStage);
		QCOMPARE(QDir(m_tempDir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
	}
	pair.bob.m_syncSaveFile        = fileSync;
	pair.bob.m_syncSavePublication = publicationSync;
	// An already-published file needs explicit confirmation on retry too.
	pair.bob.saveTransferAs(id, target, replace || afterPublication);
	QCOMPARE(received.last().first().value< PQFT::FTTransferInfo >().state, PQFT::FTTransferInfo::State::Saved);
	QFile output(target);
	QVERIFY(output.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), expected);
}

void TestFileTransferEngine::simultaneousSendsShareRateLimit() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 64 * 1024;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	QElapsedTimer elapsed;
	quint64 sentBytes = 0;
	QString violation;
	QSet< QByteArray > delivered;
	QList< QByteArray > deliveryOrder;
	pair.onChunkDelivered = [&](const QByteArray &id, quint64, quint64, const QByteArray &data) {
		delivered.insert(id);
		deliveryOrder.append(id);
		sentBytes += static_cast< quint64 >(data.size());
		const quint64 allowed = quint64(config.sendRateBytesPerSecond) * quint64(elapsed.elapsed() + 2) / 1000;
		if (sentBytes > allowed && violation.isEmpty())
			violation = QString("Concurrent jobs emitted %1 bytes in %2 ms").arg(sentBytes).arg(elapsed.elapsed());
	};
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source       = writeTestFile(64 * 1024);
	const QString secondSource = writeTestFile(80 * 1024);
	elapsed.start();
	QVERIFY(pair.alice.startSend(secondSource, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	auto finished = [&]() {
		QSet< QByteArray > ids;
		for (const auto &args : sent) {
			const auto info = args.first().value< PQFT::FTTransferInfo >();
			if (info.state == PQFT::FTTransferInfo::State::Saved)
				ids.insert(info.transferId);
		}
		return ids.size() == 2;
	};
	QTRY_VERIFY_WITH_TIMEOUT(finished(), 15000);
	QCOMPARE(delivered.size(), 2);
	QVERIFY(deliveryOrder.size() >= 2);
	QVERIFY(deliveryOrder.at(0) != deliveryOrder.at(1));
	QVERIFY2(violation.isEmpty(), qPrintable(violation));
	QSet< QByteArray > ready;
	for (const auto &args : received) {
		const auto info = args.first().value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Ready)
			ready.insert(info.transferId);
	}
	QCOMPARE(ready.size(), 2);
	for (const QByteArray &id : ready) {
		const QString target = m_tempDir.filePath(QString::fromLatin1(id.toHex()) + ".bin");
		pair.bob.saveTransferAs(id, target);
		QFile output(target);
		QVERIFY(output.open(QIODevice::ReadOnly));
		QFile input(output.size() == 64 * 1024 ? source : secondSource);
		QVERIFY(input.open(QIODevice::ReadOnly));
		QCOMPARE(output.readAll(), input.readAll());
	}
}

void TestFileTransferEngine::unlimitedSendYieldsToQueuedAbort() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	int chunks            = 0;
	pair.onChunkDelivered = [&](const QByteArray &id, quint64, quint64, const QByteArray &) {
		if (++chunks == 1)
			QTimer::singleShot(0, &pair.alice, [&, id]() { pair.alice.abortTransfer(id); });
	};
	QSignalSpy updates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 16 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Aborted), 10000);
	QVERIFY(chunks > 0 && chunks < 64);
	QVERIFY(!sawState(updates, {}, PQFT::FTTransferInfo::State::Saved));
}

void TestFileTransferEngine::synchronousChunkAbortStaysAborted() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(config);
	pair.onChunkDelivered = [&](const QByteArray &id, quint64, quint64, const QByteArray &) {
		pair.alice.abortTransfer(id);
	};
	QSignalSpy updates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Aborted), 10000);
	QVERIFY(!sawState(updates, {}, PQFT::FTTransferInfo::State::Saved));
}

void TestFileTransferEngine::abortingAnotherJobDoesNotStopPacing() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 64 * 1024;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	pair.alice.setTransport(
		[&](unsigned int target, const QByteArray &frame) {
			if (target == BobSession)
				pair.bob.onControlMessage(AliceSession, frame);
		},
		[&](const QByteArray &id, quint64 index, quint64 count, const QByteArray &data) {
			if (pair.onChunkDelivered)
				pair.onChunkDelivered(id, index, count, data);
			pair.bob.onDataMessage(AliceSession, id, index, count, data);
		});
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(64 * 1024);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { 3, m_bobFp } }) > 0);
	QByteArray waiting;
	for (const auto &args : sent) {
		const auto info = args.first().value< PQFT::FTTransferInfo >();
		if (info.peerSession == 3)
			waiting = info.transferId;
	}
	QVERIFY(!waiting.isEmpty());
	bool cancelled        = false;
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &) {
		if (!cancelled) {
			cancelled = true;
			pair.alice.abortTransfer(waiting);
		}
	};
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 3000);
	QVERIFY(cancelled);
	QVERIFY(sawState(sent, waiting, PQFT::FTTransferInfo::State::Aborted));
}

void TestFileTransferEngine::synchronousCompletionAbortStaysAborted() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(config);
	QByteArray completeId;
	pair.onChunkDelivered = [&](const QByteArray &id, quint64 index, quint64 count, const QByteArray &) {
		if (index + 1 == count)
			completeId = id;
	};
	pair.alice.setTransport(
		[&](unsigned int, const QByteArray &frame) {
			if (!completeId.isEmpty()) {
				const QByteArray id = completeId;
				completeId.clear();
				pair.alice.abortTransfer(id);
				return;
			}
			pair.bob.onControlMessage(AliceSession, frame);
		},
		[&](const QByteArray &id, quint64 index, quint64 count, const QByteArray &data) {
			pair.onChunkDelivered(id, index, count, data);
			pair.bob.onDataMessage(AliceSession, id, index, count, data);
		});
	QSignalSpy updates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(64 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Aborted), 10000);
	QVERIFY(!sawState(updates, {}, PQFT::FTTransferInfo::State::Saved));
}

void TestFileTransferEngine::pausedDeliveryDoesNotReleaseUnboundedCredit() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 16 * 1024 * 1024;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	QElapsedTimer elapsed;
	QList< QPair< qint64, quint64 > > deliveries;
	quint64 total = 0;
	QString violation;
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &data) {
		total += static_cast< quint64 >(data.size());
		const qint64 now    = elapsed.elapsed();
		const quint64 burst = quint64(config.chunkSize) + PQFT::TagSize + config.sendRateBytesPerSecond / 20;
		for (const auto &previous : deliveries) {
			const quint64 allowed =
				burst + quint64(config.sendRateBytesPerSecond) * quint64(now - previous.first + 2) / 1000;
			if (total - previous.second > allowed && violation.isEmpty())
				violation = QString("Paused callback released %1 bytes in %2 ms")
								.arg(total - previous.second)
								.arg(now - previous.first);
		}
		deliveries.append({ now, total });
		if (deliveries.size() == 1)
			QTest::qSleep(300);
	};
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	elapsed.start();
	QVERIFY(pair.alice.startSend(writeTestFile(4 * 1024 * 1024), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	QVERIFY2(violation.isEmpty(), qPrintable(violation));
}

namespace {
QString stagingDirectory(const QByteArray &transfer) {
	const QDir root(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
	const QString prefix      = "mumble-ft-" + QString::fromLatin1(transfer.toHex()) + "-";
	const QStringList matches = root.entryList({ prefix + "*" }, QDir::Dirs | QDir::NoDotAndDotDot);
	if (matches.size() == 1)
		return root.filePath(matches.first());
	// Also observe the unfixed layout when running the negative control.
	return root.filePath("mumble-ft/" + QString::fromLatin1(transfer.toHex()));
}
} // namespace

void TestFileTransferEngine::privateReceiveStorage_data() {
	QTest::addColumn< bool >("passwordMode");
	QTest::newRow("plaintext") << false;
	QTest::newRow("password-spool") << true;
}

void TestFileTransferEngine::privateReceiveStorage() {
	QFETCH(bool, passwordMode);
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(config);
	QSignalSpy updates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(80 * 1024);
	QVERIFY(!source.isEmpty());
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", passwordMode,
								 QByteArray("private fixture password"), { { BobSession, m_bobFp } })
			> 0);
	QTRY_VERIFY_WITH_TIMEOUT(
		sawState(sent, {}, passwordMode ? PQFT::FTTransferInfo::State::Verifying : PQFT::FTTransferInfo::State::Saved),
		10000);
	if (passwordMode)
		QVERIFY(!sawState(sent, {}, PQFT::FTTransferInfo::State::Saved));
	QByteArray transfer;
	for (const auto &args : updates) {
		const auto info = args.at(0).value< PQFT::FTTransferInfo >();
		if (info.state
			== (passwordMode ? PQFT::FTTransferInfo::State::WaitingPassword : PQFT::FTTransferInfo::State::Ready))
			transfer = info.transferId;
	}
	QVERIFY(!transfer.isEmpty());
	const QString directory = stagingDirectory(transfer);
	QVERIFY(QFile::exists(directory + "/content.bin"));
	const QString prefix = "mumble-ft-" + QString::fromLatin1(transfer.toHex()) + "-";
	QVERIFY2(QFileInfo(directory).fileName().startsWith(prefix), "Receive directory is predictable");
	QVERIFY(QFileInfo(directory).fileName().size() > prefix.size());
#ifdef Q_OS_UNIX
	const auto shared =
		QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
	QVERIFY(!(QFileInfo(directory).permissions() & shared));
	QVERIFY(!(QFileInfo(directory + "/content.bin").permissions() & shared));
	if (passwordMode) {
		QVERIFY(QFile::exists(directory + "/chunks.spool"));
		QVERIFY(!(QFileInfo(directory + "/chunks.spool").permissions() & shared));
	}
#endif
	pair.bob.abortTransfer(transfer);
	QVERIFY(!QDir(directory).exists());
}

void TestFileTransferEngine::predictableReceivePathCannotRedirectWrites() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(config);
	QList< QByteArray > controls;
	pair.alice.setTransport([&](unsigned int, const QByteArray &frame) { controls.append(frame); },
							[&](const QByteArray &id, quint64 index, quint64 total, const QByteArray &data) {
								pair.bob.onDataMessage(AliceSession, id, index, total, data);
							});
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(1024);
	QVERIFY(!source.isEmpty());
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QVERIFY(!sent.isEmpty());
	const QByteArray transfer = sent.first().first().value< PQFT::FTTransferInfo >().transferId;
	const QString legacy      = QDir::temp().filePath("mumble-ft/" + QString::fromLatin1(transfer.toHex()));
	QVERIFY(!QFileInfo::exists(legacy));
	QVERIFY(QDir().mkpath(legacy));
	const auto cleanup = qScopeGuard([&]() { QDir(legacy).removeRecursively(); });
	const QByteArray sentinel("existing private fixture; do not modify");
	const QString victimPath = m_tempDir.filePath("precreation-victim.bin");
	QFile victim(victimPath);
	QVERIFY(victim.open(QIODevice::WriteOnly));
	QCOMPARE(victim.write(sentinel), static_cast< qint64 >(sentinel.size()));
	victim.close();
#ifdef Q_OS_UNIX
	QVERIFY(QFile::link(victimPath, legacy + "/content.bin"));
#else
	QVERIFY(QFile::copy(victimPath, legacy + "/content.bin"));
#endif
	while (!controls.isEmpty())
		pair.bob.onControlMessage(AliceSession, controls.takeFirst());
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, transfer, PQFT::FTTransferInfo::State::Ready), 10000);
	QFile precreated(legacy + "/content.bin");
	QVERIFY(precreated.open(QIODevice::ReadOnly));
	QCOMPARE(precreated.readAll(), sentinel);
	QVERIFY(victim.open(QIODevice::ReadOnly));
	QCOMPARE(victim.readAll(), sentinel);
	pair.bob.abortTransfer(transfer);
	QVERIFY(QFileInfo::exists(legacy + "/content.bin"));
}

void TestFileTransferEngine::earlyChunksKeepTheirTransferIdentity() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 0;
	config.chunkSize              = 16 * 1024;
	pair.alice.setConfig(config);
	QList< QByteArray > controls;
	pair.alice.setTransport([&](unsigned int, const QByteArray &frame) { controls.append(frame); },
							[&](const QByteArray &id, quint64 index, quint64 total, const QByteArray &data) {
								pair.bob.onDataMessage(AliceSession, id, index, total, data);
							});
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(32 * 1024);
	QVERIFY(!source.isEmpty());
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	const QByteArray transfer = sent.first().first().value< PQFT::FTTransferInfo >().transferId;
	// Complete M1/M3 while deliberately withholding the manifest.
	while (!controls.isEmpty()) {
		quint8 type = 0;
		QVERIFY(PQFT::FTFrame::decodeHeader(controls.first(), type));
		if (type == PQFT::FTFrame::TypeManifest)
			break;
		pair.bob.onControlMessage(AliceSession, controls.takeFirst());
	}
	QVERIFY(!controls.isEmpty());
	const QByteArray unrelated = PQFT::randomBytes(PQFT::TransferIdSize);
	QVERIFY(unrelated != transfer);
	pair.bob.onDataMessage(AliceSession, unrelated, 0, 1, QByteArray(100, 'x'));
	// Legitimate early chunks must still survive until their own manifest arrives.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, transfer, PQFT::FTTransferInfo::State::Verifying), 3000);
	QVERIFY(!sawState(sent, transfer, PQFT::FTTransferInfo::State::Saved));
	while (!controls.isEmpty())
		pair.bob.onControlMessage(AliceSession, controls.takeFirst());
	QVERIFY(sawState(received, transfer, PQFT::FTTransferInfo::State::Ready));
	QVERIFY(!sawState(received, transfer, PQFT::FTTransferInfo::State::Failed));
	const QString target = m_tempDir.filePath("early-chunks-received.bin");
	pair.bob.saveTransferAs(transfer, target);
	QFile original(source), output(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(output.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), original.readAll());
}

void TestFileTransferEngine::unrelatedChunksDoNotExtendPendingReceive() {
	PQFT::FileTransferEngine engine;
	engine.setIdentity(m_bob.publicKey,
					   [&](QByteArray &signature, const QByteArray &message, const QByteArray &context) {
		PQFT::SigMLDSA65 sign;
		return sign.sign(signature, m_bob.secretKey, message, context);
	});
	engine.setPinLookup([&](unsigned int) { return m_aliceFp; });
	engine.setTransport([](unsigned int, const QByteArray &) {}, {});
	PQFT::FileTransferEngine::Config config;
	config.receiveIdleTimeoutMSecs          = 100;
	config.handshakeTimeoutMSecs            = 100;
	config.identityVerificationTimeoutMSecs = 100;
	engine.setConfig(config);
	QSignalSpy updates(&engine, &PQFT::FileTransferEngine::transferUpdated);
	PQFT::FileTransferSession sender(PQFT::FileTransferSession::Role::Initiator, { m_alice.publicKey, {} }, m_bobFp);
	engine.onControlMessage(AliceSession, sender.buildM1());
	QTimer flood;
	connect(&flood, &QTimer::timeout, &engine, [&]() {
		engine.onDataMessage(AliceSession, PQFT::randomBytes(PQFT::TransferIdSize), 0, 1, QByteArray(100, 'x'));
	});
	flood.start(10);
	QTest::qWait(350);
	QVERIFY2(sawState(updates, {}, PQFT::FTTransferInfo::State::Failed),
			 "Stray chunks kept an unfinished handshake alive");
}

void TestFileTransferEngine::selectedPinSurvivesCacheRemoval() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	auto replacement = makeIdentity();
	pair.bob.setIdentity(replacement.publicKey,
						 [&replacement](QByteArray &sig, const QByteArray &msg, const QByteArray &ctx) {
							 PQFT::SigMLDSA65 impl;
							 return impl.sign(sig, replacement.secretKey, msg, ctx);
						 });
	// The GUI selected the known Bob key. A user-list refresh removed the worker's cache entry.
	pair.alice.setPinLookup([](unsigned int) { return QByteArray(); });
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;
	pair.alice.setConfig(fast);
	int chunks            = 0;
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &) { ++chunks; };
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(
		pair.alice.startSend(writeTestFile(32768), "application/octet-stream", false, {}, { { BobSession, m_bobFp } })
		> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, {}, PQFT::FTTransferInfo::State::Failed)
								 || sawState(received, {}, PQFT::FTTransferInfo::State::Ready),
							 2000);
	QCOMPARE(chunks, 0);
	QVERIFY(sawState(sent, {}, PQFT::FTTransferInfo::State::Failed));
	QVERIFY(!sawState(received, {}, PQFT::FTTransferInfo::State::Ready));
}

void TestFileTransferEngine::incomingIdentitySurvivesPeerRename() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	QString liveName = QStringLiteral("Original sender");
	pair.bob.setPeerNameLookup([&](unsigned int) { return liveName; });
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &) {
		liveName = QStringLiteral("Renamed sender");
	};
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	const QString source = writeTestFile(32768);
	QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 10000);
	PQFT::FTTransferInfo ready;
	for (const auto &args : received) {
		const auto info = args.first().value< PQFT::FTTransferInfo >();
		if (info.state == PQFT::FTTransferInfo::State::Ready)
			ready = info;
	}
	QCOMPARE(liveName, QStringLiteral("Renamed sender"));
	QCOMPARE(ready.peerName, QStringLiteral("Original sender"));
	QCOMPARE(ready.peerFingerprint, m_aliceFp);
	const QString target = m_tempDir.filePath("identity-captured.bin");
	pair.bob.saveTransferAs(ready.transferId, target);
	const auto saved = received.last().first().value< PQFT::FTTransferInfo >();
	QCOMPARE(saved.state, PQFT::FTTransferInfo::State::Saved);
	QCOMPARE(saved.peerName, ready.peerName);
	QCOMPARE(saved.peerFingerprint, ready.peerFingerprint);
	QFile input(source), output(target);
	QVERIFY(input.open(QIODevice::ReadOnly));
	QVERIFY(output.open(QIODevice::ReadOnly));
	QCOMPARE(output.readAll(), input.readAll());
}

void TestFileTransferEngine::sendReceiveRoundTrip() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config fast;
	fast.sendRateBytesPerSecond = 0;   // unlimited: bounded batches yield to controls
	pair.alice.setConfig(fast);
	pair.bob.setConfig(fast);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(600 * 1024 + 17);   // 3 chunks at 256 KiB
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
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
		startedBytes = pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } });
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
		QVERIFY(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }) > 0);
		QTRY_VERIFY_WITH_TIMEOUT(sawState(updates, {}, PQFT::FTTransferInfo::State::Ready), 10000);
		QByteArray transfer;
		for (const auto &arguments : updates) {
			const auto info = arguments.at(0).value< PQFT::FTTransferInfo >();
			if (info.incoming && info.state == PQFT::FTTransferInfo::State::Ready) {
				transfer = info.transferId;
			}
		}
		QVERIFY(!transfer.isEmpty());
		receiveDirectory = stagingDirectory(transfer);
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
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true, QByteArray("correct horse battery staple"),
								  { { BobSession, m_bobFp } }),
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
				if (info.transferId == passwordTransfer && info.state == PQFT::FTTransferInfo::State::Ready) {
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
								  { { BobSession, m_bobFp } }),
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
				if (info.transferId == passwordTransfer && info.state == PQFT::FTTransferInfo::State::Failed) {
					failedWithGenericError = info.error.contains("Decryption failed");
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(failedWithGenericError);

	// No temp directory survived for that transfer
	const QString dir = stagingDirectory(passwordTransfer);
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
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
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
	const QString dir = stagingDirectory(failedTransfer);
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
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true, QByteArray("a late password"),
								  { { BobSession, m_bobFp } }),
			 200ull * 1024);

	// Let the sender finish uploading BEFORE any password is provided.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(aliceUpdates, QByteArray(), PQFT::FTTransferInfo::State::Verifying), 30000);
	QVERIFY(!sawState(aliceUpdates, {}, PQFT::FTTransferInfo::State::Saved));
	QVERIFY(sawState(bobUpdates, QByteArray(), PQFT::FTTransferInfo::State::WaitingPassword));

	QByteArray passwordTransfer;
	for (const auto &argument : bobPassword) {
		passwordTransfer = argument.at(0).toByteArray();
	}
	QVERIFY(!passwordTransfer.isEmpty());

	pair.bob.providePassword(passwordTransfer, QByteArray("a late password"));

	// Every spooled chunk replays and the transfer completes.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, passwordTransfer, PQFT::FTTransferInfo::State::Ready), 30000);

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
	QCOMPARE(pair.alice.startSend(first, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
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
	QCOMPARE(pair.alice.startSend(second, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
			 100ull * 1024);

	QByteArray secondTransfer;
	QTRY_VERIFY_WITH_TIMEOUT(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready && info.transferId != firstTransfer) {
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
	const QString firstDir = stagingDirectory(firstTransfer);
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
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
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
	const QString dir = stagingDirectory(readyTransfer);
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

// Both users can take longer than a network timeout to verify safety numbers.
void TestFileTransferEngine::delayedIdentityVerificationStillDelivers() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.handshakeTimeoutMSecs            = 100;
	config.identityVerificationTimeoutMSecs = 1000;
	config.receiveIdleTimeoutMSecs          = 100;
	config.sendRateBytesPerSecond           = 0;
	config.authorizeSendPeers               = true;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);
	// A sender's existing pin does not mean the receiver has pinned the sender.
	pair.bob.setPinLookup([](unsigned int) { return QByteArray(); });
	QSignalSpy receiverPrompts(&pair.bob, &PQFT::FileTransferEngine::firstContact);
	QSignalSpy senderPrompts(&pair.alice, &PQFT::FileTransferEngine::firstContact);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	int chunks            = 0;
	pair.onChunkDelivered = [&](const QByteArray &, quint64, quint64, const QByteArray &) { ++chunks; };
	const QString source  = writeTestFile(4096);
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, {}, { { BobSession, m_bobFp } }), 4096ull);
	QCOMPARE(receiverPrompts.size(), 1);
	QTest::qWait(250); // Human verification can exceed the network handshake deadline.
	QVERIFY(!sawState(sent, {}, PQFT::FTTransferInfo::State::Failed));
	QCOMPARE(chunks, 0);
	pair.bob.resolveFirstContact(AliceSession, true, receiverPrompts.first().at(5).toULongLong());
	QCOMPARE(senderPrompts.size(), 1);
	QTest::qWait(250); // The receiver must also wait for the sender's verification.
	QVERIFY(!sawState(received, {}, PQFT::FTTransferInfo::State::Failed));
	QCOMPARE(chunks, 0);
	const QByteArray transferId = senderPrompts.first().at(3).toByteArray();
	pair.alice.resolveSendAuthorization(BobSession, transferId, true);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, transferId, PQFT::FTTransferInfo::State::Ready), 5000);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, transferId, PQFT::FTTransferInfo::State::Saved), 5000);
	QVERIFY(chunks > 0);
	const QString saved = m_tempDir.filePath(QStringLiteral("delayed-verification.bin"));
	pair.bob.saveTransferAs(transferId, saved);
	QVERIFY(sawState(received, transferId, PQFT::FTTransferInfo::State::Saved));
	QFile original(source), copy(saved);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(copy.open(QIODevice::ReadOnly));
	QCOMPARE(copy.readAll(), original.readAll());
}

void TestFileTransferEngine::quickRecipientDoesNotShortenOtherVerification() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine third;
	third.setIdentity(m_bob.publicKey,
					  [&](QByteArray &signature, const QByteArray &message, const QByteArray &context) {
						  PQFT::SigMLDSA65 signer;
						  return signer.sign(signature, m_bob.secretKey, message, context);
					  });
	third.setPinLookup([](unsigned int) { return QByteArray(); });
	PQFT::FileTransferEngine::Config config;
	config.handshakeTimeoutMSecs            = 300;
	config.identityVerificationTimeoutMSecs = 500;
	config.receiveIdleTimeoutMSecs          = 100;
	config.authorizeSendPeers               = true;
	config.sendRateBytesPerSecond           = 0;
	pair.alice.setConfig(config);
	pair.bob.setConfig(config);
	third.setConfig(config);
	pair.alice.setTransport(
		[&](unsigned int target, const QByteArray &frame) {
			(target == BobSession ? pair.bob : third).onControlMessage(AliceSession, frame);
		},
		[&](const QByteArray &id, quint64 index, quint64 total, const QByteArray &data) {
			pair.bob.onDataMessage(AliceSession, id, index, total, data);
			third.onDataMessage(AliceSession, id, index, total, data);
		});
	third.setTransport(
		[&](unsigned int, const QByteArray &frame) {
			quint8 type = 0;
			PQFT::FTFrame::decodeHeader(frame, type);
			if (type == PQFT::FTFrame::TypeM2) {
				QTimer::singleShot(200, &pair.alice, [&, frame]() { pair.alice.onControlMessage(3, frame); });
			} else {
				pair.alice.onControlMessage(3, frame);
			}
		},
		{});
	connect(&pair.alice, &PQFT::FileTransferEngine::firstContact, &pair.alice,
			[&](unsigned int peer, const QByteArray &, const QString &, const QByteArray &id, bool sending) {
				if (sending)
					QTimer::singleShot(peer == BobSession ? 0 : 350, &pair.alice,
									   [&, peer, id]() { pair.alice.resolveSendAuthorization(peer, id, true); });
			});
	QSignalSpy prompts(&third, &PQFT::FileTransferEngine::firstContact);
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy earlyReceived(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&third, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(pair.alice.startSend(writeTestFile(4096), "application/octet-stream", false, {},
								 { { BobSession, m_bobFp }, { 3, m_bobFp } })
			> 0);
	QCOMPARE(prompts.size(), 1);
	QTest::qWait(350);
	third.resolveFirstContact(AliceSession, true, prompts.first().at(5).toULongLong());
	// Remote approval (350ms), network delivery (200ms), and sender approval
	// (350ms) each fit their own budget. The earliest receiver must survive
	// their combined 900ms, beyond its original 800ms pre-manifest deadline.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(received, {}, PQFT::FTTransferInfo::State::Ready), 3000);
	QVERIFY(sawState(earlyReceived, {}, PQFT::FTTransferInfo::State::Ready));
	QVERIFY(!sawState(earlyReceived, {}, PQFT::FTTransferInfo::State::Failed));
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, {}, PQFT::FTTransferInfo::State::Saved), 3000);
	QVERIFY(!sawState(sent, {}, PQFT::FTTransferInfo::State::Failed));
}

void TestFileTransferEngine::verificationTimeoutIsNormalized_data() {
	QTest::addColumn< int >("configured");
	QTest::newRow("zero") << 0;
	QTest::newRow("negative") << -1;
	QTest::newRow("minimum-int") << std::numeric_limits< int >::min();
	QTest::newRow("positive") << 25;
	QTest::newRow("maximum-int") << std::numeric_limits< int >::max();
}

void TestFileTransferEngine::verificationTimeoutIsNormalized() {
	QFETCH(int, configured);
	const int normalized = qMax(1, configured);
	PQFT::FileTransferEngine::Config config;
	config.handshakeTimeoutMSecs            = 20;
	config.identityVerificationTimeoutMSecs = configured;
	PQFT::FileTransferEngine receiver;
	receiver.setConfig(config);
	receiver.setIdentity(m_bob.publicKey, {});
	QSignalSpy prompts(&receiver, &PQFT::FileTransferEngine::firstContact);
	QSignalSpy retired(&receiver, &PQFT::FileTransferEngine::firstContactRetired);
	PQFT::FileTransferSession initiator(PQFT::FileTransferSession::Role::Initiator, { m_alice.publicKey, {} }, {});
	const QByteArray m1 = initiator.buildM1();
	receiver.onControlMessage(AliceSession, m1);
	QCOMPARE(prompts.size(), 1);
	int activeTimers = 0;
	for (QTimer *timer : receiver.findChildren< QTimer * >()) {
		if (timer->isActive()) {
			++activeTimers;
			QCOMPARE(timer->interval(), normalized);
		}
	}
	QCOMPARE(activeTimers, 1);
	if (configured != std::numeric_limits< int >::max()) {
		QTRY_COMPARE_WITH_TIMEOUT(retired.size(), 1, 1000);
		QCOMPARE(retired.first().at(1).toULongLong(), prompts.first().at(5).toULongLong());
		// Expiry must free the pending slot for a later attempt from this peer.
		receiver.onControlMessage(AliceSession, m1);
		QCOMPARE(prompts.size(), 2);
	}
	receiver.abortAll();

	PQFT::FileTransferEngine sender;
	sender.setConfig(config);
	sender.setIdentity(m_alice.publicKey, {});
	sender.setTransport([](unsigned int, const QByteArray &) {}, {});
	QSignalSpy sent(&sender, &PQFT::FileTransferEngine::transferUpdated);
	QVERIFY(sender.startSend(writeTestFile(4096), "application/octet-stream", false, {}, { { BobSession, m_bobFp } })
			> 0);
	const qint64 expected = qMin(qint64(std::numeric_limits< int >::max()), qint64(20) + normalized);
	activeTimers          = 0;
	for (QTimer *timer : sender.findChildren< QTimer * >()) {
		if (timer->isActive()) {
			++activeTimers;
			QCOMPARE(qint64(timer->interval()), expected);
		}
	}
	QCOMPARE(activeTimers, 1);
	if (configured != std::numeric_limits< int >::max())
		QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, {}, PQFT::FTTransferInfo::State::Failed), 1000);
	sender.abortAll();
}

void TestFileTransferEngine::unansweredHandshakeStillExpires() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config config;
	config.handshakeTimeoutMSecs            = 100;
	config.identityVerificationTimeoutMSecs = 200;
	pair.alice.setConfig(config);
	pair.alice.setTransport([](unsigned int, const QByteArray &) {}, {});
	QSignalSpy sent(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);
	QElapsedTimer elapsed;
	elapsed.start();
	QVERIFY(
		pair.alice.startSend(writeTestFile(4096), "application/octet-stream", false, {}, { { BobSession, m_bobFp } })
		> 0);
	QTRY_VERIFY_WITH_TIMEOUT(sawState(sent, {}, PQFT::FTTransferInfo::State::Failed), 2000);
	QVERIFY(elapsed.elapsed() >= 200);
}

// Only authenticated sender-side handshakes may pin on first observation;
// declining the receiver's unauthenticated M1 must leave nothing behind.
void TestFileTransferEngine::firstContactPinFlags() {
	{
		// Receiver side: unknown peer parks the handshake and reports
		// pinOnObservation = false; declining drops it without an answer.
		EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
		PQFT::FileTransferEngine::Config config;
		config.sendRateBytesPerSecond           = 0;
		config.handshakeTimeoutMSecs            = 500;
		config.identityVerificationTimeoutMSecs = 100;
		pair.alice.setConfig(config);
		pair.bob.setConfig(config);
		pair.bob.setPinLookup([](unsigned int) { return QByteArray(); }); // first contact

		QSignalSpy bobFirstContact(&pair.bob, &PQFT::FileTransferEngine::firstContact);
		QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

		const QString source = writeTestFile(64 * 1024);
		QVERIFY(!source.isEmpty());
		QCOMPARE(
			pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
			64ull * 1024);

		QTRY_VERIFY_WITH_TIMEOUT(bobFirstContact.count() >= 1, 10000);
		QCOMPARE(bobFirstContact.first().at(4).toBool(), false);

		pair.bob.resolveFirstContact(AliceSession, false, bobFirstContact.first().at(5).toULongLong()); // declined

		// No M2 was ever sent: the initiator fails once its handshake
		// timeout expires instead of hanging.
		QTRY_VERIFY_WITH_TIMEOUT(sawState(aliceUpdates, QByteArray(), PQFT::FTTransferInfo::State::Failed), 10000);
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
		QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, {} } }),
				 64ull * 1024);

		QTRY_VERIFY_WITH_TIMEOUT(aliceFirstContact.count() >= 1, 30000);
		QCOMPARE(aliceFirstContact.first().at(4).toBool(), true);
		QVERIFY(!sawState(bobUpdates, QByteArray(), PQFT::FTTransferInfo::State::Ready));
		pair.alice.resolveSendAuthorization(BobSession, aliceFirstContact.first().at(3).toByteArray(), true);
		QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, QByteArray(), PQFT::FTTransferInfo::State::Ready), 30000);
	}
}

// Review regression: an incoming ABORT travels onControlMessage ->
// processControlForReceive -> cleanupReceive, where the job pointer handed
// down used to be a reference to the map's own shared_ptr — erasing the map
// entry destroyed the job mid-cleanup (use-after-free on job.spool).
void TestFileTransferEngine::incomingAbortCleansUpSafely() {
	EnginePair pair(m_alice, m_bob, m_aliceFp, m_bobFp);
	PQFT::FileTransferEngine::Config trickled;
	trickled.sendRateBytesPerSecond = 32 * 1024; // about one 16 KiB chunk per half second
	trickled.chunkSize              = 16 * 1024;
	pair.alice.setConfig(trickled);
	pair.bob.setConfig(trickled);

	QSignalSpy bobUpdates(&pair.bob, &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy aliceUpdates(&pair.alice, &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile(200 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", false, QByteArray(), { { BobSession, m_bobFp } }),
			 200ull * 1024);

	// Wait until the transfer is live (manifest verified, chunks trickling),
	// then abort from the sender side.
	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, QByteArray(), PQFT::FTTransferInfo::State::Transferring), 30000);
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

	QTRY_VERIFY_WITH_TIMEOUT(sawState(bobUpdates, transferId, PQFT::FTTransferInfo::State::Aborted), 10000);
	QVERIFY(sawState(aliceUpdates, transferId, PQFT::FTTransferInfo::State::Aborted));

	// Partial output is gone.
	const QString dir = stagingDirectory(transferId);
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
	QCOMPARE(pair.alice.startSend(source, "application/octet-stream", true, QByteArray("some password"),
								  { { BobSession, m_bobFp } }),
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
				if (info.transferId == passwordTransfer && info.state == PQFT::FTTransferInfo::State::Failed) {
					failedWithGenericError = info.error.contains("Decryption failed");
					return true;
				}
			}
			return false;
		}(),
		30000);
	QVERIFY(failedWithGenericError);

	const QString dir = stagingDirectory(passwordTransfer);
	QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir + "/content.bin") && !QFile::exists(dir + "/chunks.spool"), 10000);
}

QTEST_MAIN(TestFileTransferEngine)
#include "TestFileTransferEngine.moc"
