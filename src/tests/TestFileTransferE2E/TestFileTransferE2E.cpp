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
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSslSocket>
#include <QSqlDatabase>
#include <QTcpSocket>
#include <QTemporaryDir>
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
	}

	~E2EClient() {
		// Members destroy in reverse declaration order (engine, trust,
		// identity, then the DB handle), so the connection's users are gone
		// by the time the handle closes.
		m_socket->abort();
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
			[this](const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) {
				MumbleProto::FileData msg;
				msg.set_transfer_id(transferId.constData(), static_cast< size_t >(transferId.size()));
				msg.set_chunk_index(index);
				msg.set_chunk_count(total);
				msg.set_data(data.constData(), static_cast< size_t >(data.size()));
				sendMessage(Mumble::Protocol::TCPMessageType::FileData, msg);
			});
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
	quint64 receivedFileData	= 0;

private:
	template< typename Msg >
	bool sendMessage(Mumble::Protocol::TCPMessageType type, const Msg &msg) {
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

private:
	bool startMurmur();
	bool waitFor(const std::function< bool() > &predicate, int timeoutMSecs);
	QString writeTestFile(const QString &name, qsizetype size);

	QTemporaryDir m_tempDir;
	QDir m_serverDir;
	QProcess m_murmur;
};

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
									  { bob.sessionOf("bob") }),
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
	QCOMPARE(alice.engine().startSend(source, "application/octet-stream", true,
									  QByteArray("e2e file password"), { bob.sessionOf("dave") }),
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
	// eve never announces the capability

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
									  { bob.sessionOf("frank") }),
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

	// The plain client saw nothing at all
	QCOMPARE(eve.receivedFileControl, quint64(0));
	QCOMPARE(eve.receivedFileData, quint64(0));
	QVERIFY(!readyTransfer.isEmpty());
}

void TestFileTransferE2E::cleanupTestCase() {
	m_murmur.terminate();
	m_murmur.waitForFinished(5000);
}

QTEST_MAIN(TestFileTransferE2E)
#include "TestFileTransferE2E.moc"
