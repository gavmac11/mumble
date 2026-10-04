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
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
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
} // namespace

class TestFileTransferEngine : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void cleanupTestCase();

	void sendReceiveRoundTrip();
	void passwordRoundTrip();
	void wrongPasswordFailsClosed();
	void duplicateChunkIsFatal();

private:
	QString writeTestFile(qsizetype size);

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

void TestFileTransferEngine::cleanupTestCase() { }

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

QTEST_MAIN(TestFileTransferEngine)
#include "TestFileTransferEngine.moc"
