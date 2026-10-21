// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE/>.
//
// End-to-end file-transfer test: a real murmur on a scratch port and two
// (for the gating case: three) full client connections speaking the Mumble
// protobuf protocol over TLS — no GUI. Both transfer participants run the
// real identity / trust-store / engine stack, so the transfer crosses the
// server's blind-relay path (routing, actor rewriting, capability gating)
// exactly as it would in the shipped client. The dialog flows (identity
// creation, safety-number verification) are applied directly, which is what
// those dialogs do.

#include <QTest>

#include "Mumble.pb.h"
#include "MumbleProtocol.h"
#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/engine/FileTransferEngine.h"
#include "PQFileTransfer/identity/FTIdentity.h"
#include "PQFileTransfer/identity/PeerTrustStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

#include <memory>

using PQFT::FileTransferIdentity;
using PQFT::PeerTrustStore;

namespace {
constexpr quint16 MurmurPort = 52187;

/// One full client: TLS connection, own scratch DB, identity, trust store
/// and engine — everything the shipped client runs below the dialogs.
class E2EClient {
public:
	E2EClient(QString name, const QString &dbPath, QObject *context)
		: m_name(std::move(name)), m_socket(new QSslSocket(context)) {
		m_db = QSqlDatabase::addDatabase("QSQLITE", "ft-e2e-" + m_name);
		m_db.setDatabaseName(dbPath);
		m_db.open();
		PQFT::FileTransferIdentity::ensureSchema(m_db);
		m_identity = std::make_unique< FileTransferIdentity >(m_db);
		m_trust	= std::make_unique< PeerTrustStore >(m_db);

		QObject::connect(m_socket, &QSslSocket::readyRead, context,
				[this]() { readPending(); });

		m_engine.setPinLookup([this](quint32 peerSession) -> QByteArray {
			const QString username = m_users.value(peerSession);
			if (username.isEmpty()) {
				return QByteArray();
			}
			PQFT::PinnedPeer peer;
			return m_trust->lookup(peer, m_serverDigest, username) ? peer.fingerprint : QByteArray();
		});
		m_pingTimer.setInterval(5000);
		QObject::connect(&m_pingTimer, &QTimer::timeout, context, [this]() {
			if (m_ownSession != 0) {
				MumbleProto::Ping ping;
				ping.set_timestamp(static_cast< quint64 >(QDateTime::currentMSecsSinceEpoch()));
				sendMessage(Mumble::Protocol::TCPMessageType::Ping, ping);
			}
		});
		m_pingTimer.start();
	}

	~E2EClient() {
		// Members destroy in reverse declaration order (engine, trust,
		// identity, then the DB handle), so the connection's users are gone
		// by the time the handle closes.
		m_engine.abortAll();
		m_engine.setTransport({}, {});
		m_engine.setTargetedChunkTransport({});
		m_socket->abort();
		delete m_socket;
		m_socket = nullptr;
		m_db.close();
	}

	bool connectToServer() {
		m_socket->setPeerVerifyMode(QSslSocket::VerifyNone);
		m_socket->connectToHostEncrypted("127.0.0.1", MurmurPort);
		if (!m_socket->waitForEncrypted(15000)) {
			return false;
		}
		m_serverDigest = m_socket->peerCertificate().digest(QCryptographicHash::Sha1);

		MumbleProto::Version version;
		version.set_version_v2(static_cast< quint64 >(1) << 32);
		version.set_release("file-transfer-e2e");
		if (!sendMessage(Mumble::Protocol::TCPMessageType::Version, version)) {
			return false;
		}
		MumbleProto::Authenticate auth;
		auth.set_username(m_name.toStdString());
		return sendMessage(Mumble::Protocol::TCPMessageType::Authenticate, auth);
	}

	void announceCapability() {
		MumbleProto::UserState state;
		state.set_session(m_ownSession);
		state.set_file_transfer_capable(true);
		state.set_file_transfer_delivery_receipts(true);
		sendMessage(Mumble::Protocol::TCPMessageType::UserState, state);
	}

	/// Wire the engine's transports to this client's socket.
	void installEngine() {
		FileTransferIdentity *identity = m_identity.get();
		m_engine.setIdentity(identity->publicKey(),
							 [identity](QByteArray &sig, const QByteArray &msg,
										const QByteArray &ctx) { return identity->sign(sig, msg, ctx); });
		m_engine.setTransport(
			[this](unsigned int target, const QByteArray &payload) {
				MumbleProto::FileTransferControl msg;
				msg.add_target_session(target);
				msg.set_payload(payload.constData(), static_cast< size_t >(payload.size()));
				sendMessage(Mumble::Protocol::TCPMessageType::FileTransferControl, msg);
			},
			{});
		m_engine.setTargetedChunkTransport(
			[this](const QList< unsigned int > &targets, const QByteArray &transferId, quint64 index, quint64 total,
				   const QByteArray &data) {
				MumbleProto::FileData msg;
				for (unsigned int target : targets)
					msg.add_target_session(target);
				msg.set_transfer_id(transferId.constData(), static_cast< size_t >(transferId.size()));
				msg.set_chunk_index(index);
				msg.set_chunk_count(total);
				msg.set_data(data.constData(), static_cast< size_t >(data.size()));
				return sendMessage(Mumble::Protocol::TCPMessageType::FileData, msg);
			},
			true);
	}

	quint32 ownSession() const { return m_ownSession; }
	bool knowsPeer(const QString &name) const { return m_sessionByName.contains(name); }
	quint32 sessionOf(const QString &name) const { return m_sessionByName.value(name); }
	bool peerIsCapable(const QString &name) const {
		return m_capable.contains(m_sessionByName.value(name));
	}
	const QByteArray &serverDigest() const { return m_serverDigest; }
	FileTransferIdentity *identity() const { return m_identity.get(); }
	PeerTrustStore *trust() const { return m_trust.get(); }
	PQFT::FileTransferEngine &engine() { return m_engine; }

	quint64 receivedFileControl = 0;
	quint64 receivedFileData    = 0;
	quint64 relayRetries        = 0;
	quint64 relayRefusals       = 0;

	bool sendFileData(const MumbleProto::FileData &msg) {
		return sendMessage(Mumble::Protocol::TCPMessageType::FileData, msg);
	}

private:
	template< typename Msg > bool sendMessage(Mumble::Protocol::TCPMessageType type, const Msg &msg) {
		QByteArray payload(static_cast< int >(msg.ByteSizeLong()), Qt::Uninitialized);
		msg.SerializeWithCachedSizesToArray(reinterpret_cast< unsigned char * >(payload.data()));
		return sendMessage(type, payload);
	}

	bool sendMessage(Mumble::Protocol::TCPMessageType type, const QByteArray &payload) {
		QByteArray frame(6 + payload.size(), Qt::Uninitialized);
		qToBigEndian(static_cast< quint16 >(type), frame.data());
		qToBigEndian(static_cast< quint32 >(payload.size()), frame.data() + 2);
		memcpy(frame.data() + 6, payload.constData(), static_cast< size_t >(payload.size()));
		return m_socket->write(frame) == frame.size();
	}

	void readPending() {
		m_buffer.append(m_socket->readAll());
		while (m_buffer.size() >= 6) {
			const quint16 type = qFromBigEndian< quint16 >(
				reinterpret_cast< const unsigned char * >(m_buffer.constData()));
			const quint32 length = qFromBigEndian< quint32 >(
				reinterpret_cast< const unsigned char * >(m_buffer.constData()) + 2);
			if (static_cast< quint32 >(m_buffer.size()) < 6 + length) {
				break;
			}
			const QByteArray payload = m_buffer.mid(6, static_cast< int >(length));
			m_buffer.remove(0, static_cast< int >(6 + length));
			handleFrame(static_cast< Mumble::Protocol::TCPMessageType >(type), payload);
		}
	}

	void handleFrame(Mumble::Protocol::TCPMessageType type, const QByteArray &payload) {
		switch (type) {
			case Mumble::Protocol::TCPMessageType::ServerSync: {
				MumbleProto::ServerSync msg;
				if (msg.ParseFromArray(payload.constData(), static_cast< int >(payload.size()))) {
					m_ownSession = msg.session();
				}
				break;
			}
			case Mumble::Protocol::TCPMessageType::UserState: {
				MumbleProto::UserState msg;
				if (msg.ParseFromArray(payload.constData(), static_cast< int >(payload.size()))) {
					if (msg.has_name()) {
						const QString name = QString::fromStdString(msg.name());
						m_users.insert(msg.session(), name);
						m_sessionByName.insert(name, msg.session());
					}
					if (msg.has_file_transfer_capable()) {
						if (msg.file_transfer_capable()) {
							m_capable.insert(msg.session());
						} else {
							m_capable.remove(msg.session());
						}
					}
				}
				break;
			}
			case Mumble::Protocol::TCPMessageType::FileTransferControl: {
				++receivedFileControl;
				MumbleProto::FileTransferControl msg;
				if (msg.ParseFromArray(payload.constData(), static_cast< int >(payload.size()))
					&& msg.has_actor() && msg.has_payload()) {
					m_engine.onControlMessage(
						msg.actor(),
						QByteArray(msg.payload().data(), static_cast< int >(msg.payload().size())));
				}
				break;
			}
			case Mumble::Protocol::TCPMessageType::FileData: {
				++receivedFileData;
				MumbleProto::FileData msg;
				if (msg.ParseFromArray(payload.constData(), static_cast< int >(payload.size()))
					&& msg.has_actor() && msg.has_transfer_id() && msg.has_data()) {
					if (msg.has_relay_accepted() || msg.has_relay_retry_after_ms()) {
						if (msg.relay_retry_after_ms() > 0)
							++relayRetries;
						m_engine.relayResponse(
							QByteArray(msg.transfer_id().data(), static_cast< int >(msg.transfer_id().size())),
							msg.chunk_index(), msg.relay_accepted(), msg.relay_retry_after_ms(),
							QString::fromStdString(msg.relay_error()));
						break;
					}
					if (msg.has_relay_error()) {
						++relayRefusals;
						m_engine.relayFailed(
							QByteArray(msg.transfer_id().data(), static_cast< int >(msg.transfer_id().size())),
							QString::fromStdString(msg.relay_error()));
						break;
					}
					m_engine.onDataMessage(
						msg.actor(),
						QByteArray(msg.transfer_id().data(),
								   static_cast< int >(msg.transfer_id().size())),
						msg.chunk_index(), msg.has_chunk_count() ? msg.chunk_count() : 0,
						QByteArray(msg.data().data(), static_cast< int >(msg.data().size())));
				}
				break;
			}
			default:
				break;
		}
	}

	QTimer m_pingTimer;
	QString m_name;
	QSslSocket *m_socket = nullptr;
	QByteArray m_buffer;
	quint32 m_ownSession = 0;
	QHash< quint32, QString > m_users;
	QHash< QString, quint32 > m_sessionByName;
	QSet< quint32 > m_capable;
	QByteArray m_serverDigest;
	QSqlDatabase m_db;
	std::unique_ptr< FileTransferIdentity > m_identity;
	std::unique_ptr< PeerTrustStore > m_trust;
	PQFT::FileTransferEngine m_engine;
};
} // namespace

class TestFileTransferE2E : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void cleanupTestCase();

	void endToEndTransfer();
	void endToEndPasswordTransfer();
	void capabilityGating();
	void missingChunkIndexIsRefused();
	void concurrentTargetedLargeDelivery();
	void competingSendersRetryAggregateBudget();

private:
	bool startMurmur();
	bool waitFor(const std::function< bool() > &predicate, int timeoutMSecs);
	QString writeTestFile(const QString &name, qsizetype size);

	QTemporaryDir m_tempDir;
	QDir m_serverDir;
	QProcess m_murmur;
};

void TestFileTransferE2E::missingChunkIndexIsRefused() {
	E2EClient sender("index-sender", m_tempDir.filePath("index-sender.sqlite"), this);
	E2EClient receiver("index-receiver", m_tempDir.filePath("index-receiver.sqlite"), this);
	QVERIFY(sender.connectToServer());
	QVERIFY(receiver.connectToServer());
	QVERIFY(waitFor([&]() { return sender.ownSession() && receiver.ownSession(); }, 10000));
	sender.announceCapability();
	receiver.announceCapability();
	QVERIFY(waitFor([&]() { return sender.peerIsCapable("index-receiver"); }, 10000));

	MumbleProto::FileData data;
	data.set_transfer_id(std::string(16, 'i'));
	data.set_data(std::string(16, 'd'));
	data.add_target_session(receiver.ownSession());
	QVERIFY(sender.sendFileData(data));
	QVERIFY(waitFor([&]() { return sender.relayRefusals == 1; }, 10000));
	QCOMPARE(receiver.receivedFileData, 0ULL);

	// Explicit zero is the first chunk, whereas absence is malformed.
	data.set_chunk_index(0);
	QVERIFY(sender.sendFileData(data));
	QVERIFY(waitFor([&]() { return receiver.receivedFileData == 1; }, 10000));
	QCOMPARE(sender.relayRefusals, 1ULL);
}

void TestFileTransferE2E::initTestCase() {
	QVERIFY(m_tempDir.isValid());
	m_serverDir = QDir(m_tempDir.filePath("server"));
	QVERIFY(m_serverDir.mkpath("."));
	QVERIFY(startMurmur());
}

bool TestFileTransferE2E::startMurmur() {
	const QString iniPath = m_serverDir.filePath("murmur.ini");
	{
		QFile ini(iniPath);
		if (!ini.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return false;
		}
		ini.write(QStringLiteral("database=%1/murmur.sqlite\n"
								 "port=%2\n"
								 "autobanAttempts=0\n"
								 "autobanTimeframe=0\n"
								 "filebandwidthaggregate=8000000\n"
								 "bonjour=false\n")
					  .arg(m_serverDir.absolutePath())
					  .arg(MurmurPort)
					  .toUtf8());
	}

	m_murmur.setProcessChannelMode(QProcess::ForwardedChannels);
#ifdef Q_OS_WIN
	// The test runner uses the offscreen plugin, while the static Windows
	// server only imports the native platform plugin.
	QProcessEnvironment serverEnvironment = QProcessEnvironment::systemEnvironment();
	serverEnvironment.remove(QStringLiteral("QT_QPA_PLATFORM"));
	m_murmur.setProcessEnvironment(serverEnvironment);
#endif
	// Unix servers detach by default. Keep the fixture owned by QProcess so
	// startup failures are visible and cleanup cannot leave a daemon behind.
	m_murmur.start(QStringLiteral(MUMBLE_TEST_MURMUR_BINARY), { "--foreground", "--ini", iniPath });
	if (!m_murmur.waitForStarted(10000)) {
		return false;
	}

	// First start generates the server certificate; give it up to 60 s
	bool ready = false;
	waitFor(
		[this, &ready]() {
			if (m_murmur.state() == QProcess::NotRunning) {
				return true;
			}
			QTcpSocket probe;
			probe.connectToHost(QHostAddress::LocalHost, MurmurPort);
			probe.waitForConnected(200);
			ready = probe.state() == QAbstractSocket::ConnectedState;
			return ready;
		},
		60000);
	return ready;
}

bool TestFileTransferE2E::waitFor(const std::function< bool() > &predicate, int timeoutMSecs) {
	QElapsedTimer timer;
	timer.start();
	while (!predicate()) {
		if (timer.elapsed() > timeoutMSecs) {
			return false;
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(20);
	}
	return true;
}

QString TestFileTransferE2E::writeTestFile(const QString &name, qsizetype size) {
	const QString path = m_tempDir.filePath(name);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return QString();
	}
	QByteArray content(static_cast< int >(size), Qt::Uninitialized);
	for (qsizetype i = 0; i < size; ++i) {
		content[static_cast< int >(i)] = static_cast< char >((i * 31 + 7) & 0xff);
	}
	file.write(content);
	file.close();
	return path;
}

void TestFileTransferE2E::endToEndTransfer() {
	E2EClient alice("alice", m_tempDir.filePath("alice.sqlite"), this);
	E2EClient bob("bob", m_tempDir.filePath("bob.sqlite"), this);

	QVERIFY(alice.connectToServer());
	QVERIFY(waitFor([&]() { return alice.ownSession() != 0; }, 15000));
	alice.announceCapability();

	QVERIFY(bob.connectToServer());
	QVERIFY(waitFor([&]() { return bob.ownSession() != 0; }, 15000));
	bob.announceCapability();

	QVERIFY(waitFor([&]() { return alice.knowsPeer("bob") && bob.knowsPeer("alice"); }, 15000));
	QVERIFY(waitFor([&]() { return alice.peerIsCapable("bob") && bob.peerIsCapable("alice"); },
					15000));

	// Identities + cross-pin as verified peers (what the dialogs do)
	QVERIFY(alice.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	QVERIFY(bob.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	const QByteArray aliceFp = alice.identity()->fingerprint();
	const QByteArray bobFp   = bob.identity()->fingerprint();
	alice.trust()->checkAndPin(alice.serverDigest(), "bob", bobFp,
							   PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(alice.trust()->markVerified(alice.serverDigest(), "bob", bobFp));
	bob.trust()->checkAndPin(bob.serverDigest(), "alice", aliceFp,
							 PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(bob.trust()->markVerified(bob.serverDigest(), "alice", aliceFp));

	alice.installEngine();
	bob.installEngine();

	QSignalSpy bobUpdates(&bob.engine(), &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile("e2e-payload.bin", 1536 * 1024);   // 6 chunks
	QVERIFY(!source.isEmpty());
	QCOMPARE(alice.engine().startSend(source, "application/octet-stream", false, QByteArray(),
									  { { bob.sessionOf("bob"), bobFp } }),
			 1536ull * 1024);

	QByteArray readyTransfer;
	QVERIFY(waitFor(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready) {
					readyTransfer = info.transferId;
					return true;
				}
			}
			return false;
		},
		60000));
	QVERIFY(!readyTransfer.isEmpty());

	const QString target = m_tempDir.filePath("e2e-received.bin");
	bob.engine().saveTransferAs(readyTransfer, target);
	QVERIFY(waitFor([&]() { return QFileInfo::exists(target); }, 15000));

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.size(), received.size());
	QCOMPARE(original.readAll(), received.readAll());
}

void TestFileTransferE2E::endToEndPasswordTransfer() {
	E2EClient alice("carol", m_tempDir.filePath("carol.sqlite"), this);
	E2EClient bob("dave", m_tempDir.filePath("dave.sqlite"), this);

	QVERIFY(alice.connectToServer());
	QVERIFY(waitFor([&]() { return alice.ownSession() != 0; }, 15000));
	alice.announceCapability();

	QVERIFY(bob.connectToServer());
	QVERIFY(waitFor([&]() { return bob.ownSession() != 0; }, 15000));
	bob.announceCapability();

	QVERIFY(waitFor([&]() { return alice.knowsPeer("dave") && bob.knowsPeer("carol"); }, 15000));
	QVERIFY(waitFor([&]() { return alice.peerIsCapable("dave") && bob.peerIsCapable("carol"); },
					15000));

	QVERIFY(alice.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	QVERIFY(bob.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	const QByteArray aliceFp = alice.identity()->fingerprint();
	const QByteArray bobFp   = bob.identity()->fingerprint();
	alice.trust()->checkAndPin(alice.serverDigest(), "dave", bobFp,
							   PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(alice.trust()->markVerified(alice.serverDigest(), "dave", bobFp));
	bob.trust()->checkAndPin(bob.serverDigest(), "carol", aliceFp,
							 PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(bob.trust()->markVerified(bob.serverDigest(), "carol", aliceFp));

	alice.installEngine();
	bob.installEngine();

	QSignalSpy bobUpdates(&bob.engine(), &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy bobPassword(&bob.engine(), &PQFT::FileTransferEngine::passwordRequired);

	const QString source = writeTestFile("e2e-pw-payload.bin", 512 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(alice.engine().startSend(source, "application/octet-stream", true, QByteArray("e2e file password"),
									  { { bob.sessionOf("dave"), bobFp } }),
			 512ull * 1024);

	QByteArray passwordTransfer;
	QVERIFY(waitFor(
		[&]() {
			for (const auto &argument : bobPassword) {
				passwordTransfer = argument.at(0).toByteArray();
				return true;
			}
			return false;
		},
		60000));
	QVERIFY(!passwordTransfer.isEmpty());

	bob.engine().providePassword(passwordTransfer, QByteArray("e2e file password"));

	QByteArray readyTransfer;
	QVERIFY(waitFor(
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
		},
		60000));

	const QString target = m_tempDir.filePath("e2e-pw-received.bin");
	bob.engine().saveTransferAs(readyTransfer, target);
	QVERIFY(waitFor([&]() { return QFileInfo::exists(target); }, 15000));

	QFile original(source);
	QFile received(target);
	QVERIFY(original.open(QIODevice::ReadOnly));
	QVERIFY(received.open(QIODevice::ReadOnly));
	QCOMPARE(original.readAll(), received.readAll());
}

void TestFileTransferE2E::capabilityGating() {
	// A third, non-capable client in the channel must not receive a single
	// file-transfer message while a transfer runs.
	E2EClient alice("erin", m_tempDir.filePath("erin.sqlite"), this);
	E2EClient bob("frank", m_tempDir.filePath("frank.sqlite"), this);
	E2EClient eve("eve", m_tempDir.filePath("eve.sqlite"), this);   // plain client

	QVERIFY(alice.connectToServer());
	QVERIFY(waitFor([&]() { return alice.ownSession() != 0; }, 15000));
	alice.announceCapability();

	QVERIFY(bob.connectToServer());
	QVERIFY(waitFor([&]() { return bob.ownSession() != 0; }, 15000));
	bob.announceCapability();

	QVERIFY(eve.connectToServer());
	QVERIFY(waitFor([&]() { return eve.ownSession() != 0; }, 15000));
	eve.announceCapability(); // Capable but deliberately unselected.

	QVERIFY(waitFor([&]() { return alice.knowsPeer("frank") && alice.knowsPeer("eve"); }, 15000));
	QVERIFY(waitFor([&]() { return alice.peerIsCapable("frank"); }, 15000));

	QVERIFY(alice.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	QVERIFY(bob.identity()->createIdentity(QStringLiteral("e2e-passphrase")));
	const QByteArray aliceFp = alice.identity()->fingerprint();
	const QByteArray bobFp   = bob.identity()->fingerprint();
	alice.trust()->checkAndPin(alice.serverDigest(), "frank", bobFp,
							   PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(alice.trust()->markVerified(alice.serverDigest(), "frank", bobFp));
	bob.trust()->checkAndPin(bob.serverDigest(), "erin", aliceFp,
							 PQFT::safetyNumber(aliceFp, bobFp));
	QVERIFY(bob.trust()->markVerified(bob.serverDigest(), "erin", aliceFp));

	alice.installEngine();
	bob.installEngine();

	QSignalSpy bobUpdates(&bob.engine(), &PQFT::FileTransferEngine::transferUpdated);

	const QString source = writeTestFile("e2e-gate-payload.bin", 64 * 1024);
	QVERIFY(!source.isEmpty());
	QCOMPARE(alice.engine().startSend(source, "application/octet-stream", false, QByteArray(),
									  { { bob.sessionOf("frank"), bobFp } }),
			 64ull * 1024);

	QByteArray readyTransfer;
	QVERIFY(waitFor(
		[&]() {
			for (const auto &argument : bobUpdates) {
				const PQFT::FTTransferInfo info = argument.at(0).value< PQFT::FTTransferInfo >();
				if (info.state == PQFT::FTTransferInfo::State::Ready) {
					readyTransfer = info.transferId;
					return true;
				}
			}
			return false;
		},
		60000));

	// Even a capable unselected client receives no file chunks.
	QCOMPARE(eve.receivedFileControl, quint64(0));
	QCOMPARE(eve.receivedFileData, quint64(0));
	QVERIFY(!readyTransfer.isEmpty());
}

void TestFileTransferE2E::concurrentTargetedLargeDelivery() {
	E2EClient sender("large-sender", m_tempDir.filePath("large-sender.sqlite"), this);
	E2EClient first("large-first", m_tempDir.filePath("large-first.sqlite"), this);
	E2EClient second("large-second", m_tempDir.filePath("large-second.sqlite"), this);
	E2EClient other("large-other", m_tempDir.filePath("large-other.sqlite"), this);
	for (E2EClient *client : { &sender, &first, &second, &other }) {
		QVERIFY(client->connectToServer());
		QVERIFY(waitFor([&]() { return client->ownSession() != 0; }, 15000));
		client->announceCapability();
	}
	QVERIFY(waitFor(
		[&]() {
			return sender.peerIsCapable("large-first") && sender.peerIsCapable("large-second")
				   && sender.peerIsCapable("large-other") && first.knowsPeer("large-sender")
				   && second.knowsPeer("large-sender");
		},
		15000));
	for (E2EClient *client : { &sender, &first, &second })
		QVERIFY(client->identity()->createIdentity(QStringLiteral("large-isolated-passphrase")));
	const QByteArray senderFp = sender.identity()->fingerprint();
	for (const auto &pair :
		 { qMakePair(&first, QStringLiteral("large-first")), qMakePair(&second, QStringLiteral("large-second")) }) {
		QCOMPARE(sender.trust()->checkAndPin(sender.serverDigest(), pair.second, pair.first->identity()->fingerprint(),
											 "test"),
				 PQFT::TrustState::Pinned);
		QCOMPARE(pair.first->trust()->checkAndPin(pair.first->serverDigest(), "large-sender", senderFp, "test"),
				 PQFT::TrustState::Pinned);
	}
	for (E2EClient *client : { &sender, &first, &second })
		client->installEngine();
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 900000;
	config.aggregateSendRate      = 900000;
	config.chunkSize              = 128 * 1024;
	sender.engine().setConfig(config);
	QSignalSpy sent(&sender.engine(), &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy firstUpdates(&first.engine(), &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy secondUpdates(&second.engine(), &PQFT::FileTransferEngine::transferUpdated);
	const QString large = writeTestFile("beyond-meter-window.bin", 96 * 1024 * 1024);
	const QString small = writeTestFile("concurrent-distinct.bin", 3 * 1024 * 1024);
	QCOMPARE(sender.engine().startSend(large, "application/octet-stream", false, {},
									   { { first.ownSession(), first.identity()->fingerprint() } }),
			 96ull * 1024 * 1024);
	QCOMPARE(sender.engine().startSend(small, "application/octet-stream", false, {},
									   { { second.ownSession(), second.identity()->fingerprint() } }),
			 3ull * 1024 * 1024);
	auto readyId = [](const QSignalSpy &updates) {
		for (const auto &event : updates) {
			const auto info = event.first().value< PQFT::FTTransferInfo >();
			if (info.state == PQFT::FTTransferInfo::State::Ready)
				return info.transferId;
		}
		return QByteArray();
	};
	QVERIFY(waitFor([&]() { return !readyId(firstUpdates).isEmpty() && !readyId(secondUpdates).isEmpty(); }, 180000));
	QCOMPARE(other.receivedFileData, quint64(0));
	QCOMPARE(other.receivedFileControl, quint64(0));
	QCOMPARE(first.receivedFileData, quint64(768));
	QCOMPARE(second.receivedFileData, quint64(24));
	auto deliveredCount = [&]() {
		int delivered = 0;
		for (const auto &event : sent)
			if (event.first().value< PQFT::FTTransferInfo >().state == PQFT::FTTransferInfo::State::Saved)
				++delivered;
		return delivered;
	};
	QTRY_COMPARE_WITH_TIMEOUT(deliveredCount(), 2, 10000);
	const QString firstTarget  = m_tempDir.filePath("large-first-delivered.bin");
	const QString secondTarget = m_tempDir.filePath("large-second-delivered.bin");
	first.engine().saveTransferAs(readyId(firstUpdates), firstTarget);
	second.engine().saveTransferAs(readyId(secondUpdates), secondTarget);
	auto hash = [](const QString &path) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
			return QByteArray();
		QCryptographicHash digest(QCryptographicHash::Sha256);
		if (!digest.addData(&file))
			return QByteArray();
		return digest.result();
	};
	QVERIFY(!hash(firstTarget).isEmpty());
	QCOMPARE(hash(firstTarget), hash(large));
	QCOMPARE(hash(secondTarget), hash(small));
}

void TestFileTransferE2E::competingSendersRetryAggregateBudget() {
	E2EClient first("competing-first", m_tempDir.filePath("competing-first.sqlite"), this);
	E2EClient second("competing-second", m_tempDir.filePath("competing-second.sqlite"), this);
	E2EClient recipient("competing-recipient", m_tempDir.filePath("competing-recipient.sqlite"), this);
	for (E2EClient *client : { &first, &second, &recipient }) {
		QVERIFY(client->connectToServer());
		QVERIFY(waitFor([&]() { return client->ownSession() != 0; }, 15000));
		client->announceCapability();
		QVERIFY(client->identity()->createIdentity(QStringLiteral("competing-isolated-passphrase")));
	}
	QVERIFY(waitFor(
		[&]() {
			return first.peerIsCapable("competing-recipient") && second.peerIsCapable("competing-recipient")
				   && recipient.knowsPeer("competing-first") && recipient.knowsPeer("competing-second");
		},
		15000));
	for (const auto &pair : { qMakePair(&first, QStringLiteral("competing-first")),
							  qMakePair(&second, QStringLiteral("competing-second")) }) {
		QCOMPARE(pair.first->trust()->checkAndPin(pair.first->serverDigest(), "competing-recipient",
												  recipient.identity()->fingerprint(), "test"),
				 PQFT::TrustState::Pinned);
		QCOMPARE(recipient.trust()->checkAndPin(recipient.serverDigest(), pair.second,
												pair.first->identity()->fingerprint(), "test"),
				 PQFT::TrustState::Pinned);
	}
	for (E2EClient *client : { &first, &second, &recipient })
		client->installEngine();
	PQFT::FileTransferEngine::Config config;
	config.sendRateBytesPerSecond = 900000;
	config.aggregateSendRate      = 900000;
	config.chunkSize              = 128 * 1024;
	first.engine().setConfig(config);
	second.engine().setConfig(config);
	QSignalSpy firstUpdates(&first.engine(), &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy secondUpdates(&second.engine(), &PQFT::FileTransferEngine::transferUpdated);
	QSignalSpy received(&recipient.engine(), &PQFT::FileTransferEngine::transferUpdated);
	const QString sourceA = writeTestFile("competing-a.bin", 48 * 1024 * 1024);
	const QString sourceB = writeTestFile("competing-b.bin", 48 * 1024 * 1024);
	QVERIFY(first.engine().startSend(sourceA, "application/octet-stream", false, {},
									 { { recipient.ownSession(), recipient.identity()->fingerprint() } })
			> 0);
	QVERIFY(second.engine().startSend(sourceB, "application/octet-stream", false, {},
									  { { recipient.ownSession(), recipient.identity()->fingerprint() } })
			> 0);
	auto delivered = [](const QSignalSpy &updates) {
		for (const auto &event : updates)
			if (event.first().value< PQFT::FTTransferInfo >().state == PQFT::FTTransferInfo::State::Saved)
				return true;
		return false;
	};
	QVERIFY(waitFor([&]() { return delivered(firstUpdates) && delivered(secondUpdates); }, 180000));
	QVERIFY(first.relayRetries + second.relayRetries > 0);
	QCOMPARE(recipient.receivedFileData, quint64(768));
	QCOMPARE(first.receivedFileControl > 0 && second.receivedFileControl > 0, true);
	int readyCount = 0;
	for (const auto &event : received) {
		const auto info = event.first().value< PQFT::FTTransferInfo >();
		if (info.state != PQFT::FTTransferInfo::State::Ready)
			continue;
		const QString target = m_tempDir.filePath(QStringLiteral("competing-received-%1.bin").arg(++readyCount));
		recipient.engine().saveTransferAs(info.transferId, target);
		QFile original(info.peerSession == first.ownSession() ? sourceA : sourceB), saved(target);
		QVERIFY(original.open(QIODevice::ReadOnly));
		QVERIFY(saved.open(QIODevice::ReadOnly));
		QCryptographicHash sourceHash(QCryptographicHash::Sha256), targetHash(QCryptographicHash::Sha256);
		QVERIFY(sourceHash.addData(&original));
		QVERIFY(targetHash.addData(&saved));
		QCOMPARE(sourceHash.result(), targetHash.result());
	}
	QCOMPARE(readyCount, 2);
}

void TestFileTransferE2E::cleanupTestCase() {
	m_murmur.terminate();
	m_murmur.waitForFinished(5000);
}

QTEST_MAIN(TestFileTransferE2E)
#include "TestFileTransferE2E.moc"
