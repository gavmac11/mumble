// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "ClientUser.h"
#include "CustomElements.h"
#include "Database.h"
#include "LCD.h"
#include "Log.h"
#include "Logger.h"
#include "MainWindow.h"
#include "MumbleApplication.h"
#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/widgets/FileTransferDialogs.h"
#include "PluginManager.h"
#include "SSL.h"
#include "ServerHandler.h"
#include "UserModel.h"
#ifdef USE_CHAT_WEBM
#	include "ChatVideoPlayer.h"
#endif
#ifdef MUMBLE_TEST_MURMUR_BINARY
#	include "InlineWebMFixture.h"
#endif
#ifdef USE_OVERLAY
#	include "Overlay.h"
#endif
#include "Global.h"
#include "GlobalShortcut.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <memory>
#include <spdlog/sinks/qt_sinks.h>

// MainWindow registers actions, but a document test must not install native
// desktop keyboard/mouse hooks. The final action deletes this inert engine.
class FixtureShortcuts : public GlobalShortcutEngine {
public:
	ButtonInfo buttonInfo(const QVariant &) override { return {}; }
};

static void checkpoint(const char *message) {
	std::fprintf(stderr, "MainWindow fixture: %s\n", message);
	std::fflush(stderr);
}

// Keep the actual window/manager/log connection in a subprocess: a recursive
// resource lookup can block the GUI event loop, including an in-process timer.
static int mainWindowChild(const std::function< int(Global &, MainWindow &, int &) > &exercise = {}) {
	QTemporaryDir directory;
	if (!directory.isValid())
		return 1;
	Global global(directory.filePath(QStringLiteral("settings.json")));
	Global::g_global_struct     = &global;
	global.s.settingsLocation   = directory.filePath(QStringLiteral("settings.json"));
	global.s.qsDatabaseLocation = directory.filePath(QStringLiteral("client.sqlite"));
	QFile databaseFile(global.s.qsDatabaseLocation);
	if (!databaseFile.open(QIODevice::WriteOnly))
		return 1;
	databaseFile.close();
	global.s.bFTAutoAcceptPinned = false;
	global.s.bShortcutEnable     = false;
	global.s.bTTS                = false;
	for (auto it = global.s.qmMessages.begin(); it != global.s.qmMessages.end(); ++it)
		it.value() = Settings::LogConsole;
	// Match main.cpp's Qt warning -> spdlog -> developer-console path. Resource
	// loading warnings can otherwise behave differently from the shipped client.
	QTextBrowser developerLog;
	mumble::log::init();
	const auto sink = std::make_shared< spdlog::sinks::qt_color_sink_st >(&developerLog, 1000);
	mumble::log::addSink(sink);
	const auto restoreLogging = qScopeGuard([&sink]() {
		mumble::log::removeSink(sink);
		mumble::log::restoreQtMessageHandler();
	});
	// Exercise the shipped dark theme without discovering personal themes or
	// user.qss files. The normal Themes::apply path permits those overrides.
	QFile stylesheet(QStringLiteral(":/themes/Default/Dark.qss"));
	if (!stylesheet.open(QIODevice::ReadOnly))
		return 6;
	QDir::setSearchPaths(QStringLiteral("skin"), { QStringLiteral(":/themes/Default") });
	qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
	auto database        = std::make_unique< Database >(QStringLiteral("main-window-fixture"));
	global.db            = database.get();
	auto plugins         = std::make_unique< PluginManager >();
	global.pluginManager = plugins.get();
	// UserModel's teardown calls these even with an empty user list. Do not
	// run DeferInit or Overlay::setActive: no devices or overlay helpers start.
	auto lcd   = std::make_unique< LCD >();
	global.lcd = lcd.get();
#ifdef USE_OVERLAY
	auto overlay = std::make_unique< Overlay >();
	global.o     = overlay.get();
#endif
	GlobalShortcutEngine::engine = new FixtureShortcuts();
	checkpoint("constructing window");
	auto window = std::make_unique< MainWindow >(nullptr);
	global.mw   = window.get();
	auto log    = std::make_unique< Log >();
	global.l    = log.get();
	window->resize(800, 600);
	window->show();
	QApplication::processEvents();
	int heartbeats = 0;
	QTimer heartbeat;
	heartbeat.setInterval(1);
	QObject::connect(&heartbeat, &QTimer::timeout, [&heartbeats]() { ++heartbeats; });
	heartbeat.start();
	checkpoint("window ready");
	int result = 0;
	if (exercise) {
		result = exercise(global, *window, heartbeats);
	} else
		for (bool incoming : { false, true }) {
			PQFT::FTTransferInfo info;
			info.transferId = QByteArray(16, incoming ? 'r' : 's');
			info.fileName   = QStringLiteral("MainWindow transfer fixture.txt");
			info.fileSize   = 4096;
			info.incoming   = incoming;
			info.peerName   = QStringLiteral("Owned fixture peer");
			for (auto state : { PQFT::FTTransferInfo::State::Handshaking, PQFT::FTTransferInfo::State::Transferring,
								PQFT::FTTransferInfo::State::Ready }) {
				info.state = state;
				// Use the production signal connection installed by MainWindow, not
				// a reconstructed QTextDocument insertion sequence.
				checkpoint("emitting transfer update");
				const int previousHeartbeats = heartbeats;
				emit global.fileTransferManager->transferUpdated(info);
				QTest::qWait(20);
				if (heartbeats == previousHeartbeats)
					return 5;
				checkpoint("checking resource and layout");
				const QUrl url(QStringLiteral("data:application/mumble-file;base64,%1")
								   .arg(QString::fromLatin1(info.transferId.toBase64())));
				auto *document = window->qteLog->document();
				if (document->resource(QTextDocument::ImageResource, url).value< QImage >().isNull())
					return 2;
				if (!document->documentLayout()->documentSize().isValid())
					return 3;
				if (!document->toHtml().contains(QString::fromLatin1(info.transferId.toBase64())))
					return 4;
			}
		}
	checkpoint("teardown");
	window.reset();
	global.mw                  = nullptr;
	global.fileTransferManager = nullptr;
	lcd.reset();
	global.lcd = nullptr;
#ifdef USE_OVERLAY
	overlay.reset();
	global.o = nullptr;
#endif
	log.reset();
	global.l = nullptr;
	plugins.reset();
	global.pluginManager = nullptr;
	database.reset();
	global.db = nullptr;
	checkpoint("complete");
	return result;
}

#ifdef MUMBLE_TEST_MURMUR_BINARY
static bool waitFor(const std::function< bool() > &ready, int timeout = 15000) {
	QElapsedTimer elapsed;
	elapsed.start();
	while (!ready()) {
		if (elapsed.elapsed() >= timeout)
			return false;
		QTest::qWait(10);
	}
	return true;
}

static bool writeFixtureFile(const QString &path, const QByteArray &bytes) {
	QSaveFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

static QByteArray readFixtureFile(const QString &path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static QUrl embeddedWebM(QTextDocument &document) {
	for (auto block = document.begin(); block.isValid(); block = block.next()) {
		for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
			const auto text = fragment.fragment();
			if (text.isValid() && text.charFormat().isImageFormat()) {
				const QUrl url(text.charFormat().toImageFormat().name());
				if (url.toString().startsWith(QStringLiteral("data:video/webm;")))
					return url;
			}
		}
	}
	return {};
}

static void dropLocalWebM(MainWindow &window, const QString &path) {
	QMimeData mime;
	mime.setUrls({ QUrl::fromLocalFile(path) });
	QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
	checkpoint("dispatching synthetic Qt WebM drag-enter and drop to chat viewport");
	QApplication::sendEvent(window.qteChat->viewport(), &enter);
	QApplication::sendEvent(window.qteChat->viewport(), &drop);
	checkpoint("WebM drop returned");
}

static int inlineWebMDrop(Global &global, MainWindow &window, const QDir &shared, bool sending, int &heartbeats) {
	const QString role = sending ? QStringLiteral("sender") : QStringLiteral("receiver");
	const QString peer = sending ? QStringLiteral("receiver") : QStringLiteral("sender");
	auto *document     = qobject_cast< LogDocument * >(window.qteLog->document());
	if (!document || !waitFor([&]() { return global.uiImageLength == 131072u; }))
		return 30;
	QSignalSpy sent(window.qteChat, &ChatbarTextEdit::pastedImage);
	QSignalSpy fileTransfer(window.qteChat, &ChatbarTextEdit::fileDropRequested);
	QElapsedTimer gap;
	gap.start();
	qint64 maximumGap = 0;
	QTimer heartbeat;
	heartbeat.setInterval(10);
	QObject::connect(&heartbeat, &QTimer::timeout, [&]() { maximumGap = qMax(maximumGap, gap.restart()); });
	heartbeat.start();
	const int initialHeartbeats = heartbeats;
	if (!writeFixtureFile(shared.filePath(role + ".ready"), "ready"))
		return 31;
	if (sending) {
		if (!waitFor([&]() { return QFile::exists(shared.filePath(peer + ".ready")); }))
			return 32;
		dropLocalWebM(window, shared.filePath(QStringLiteral("source.webm")));
		QTest::qWait(200);
		if (sent.size() != 1 || fileTransfer.size() != 0)
			return 33;
	}
	{
		if (!waitFor([&]() { return !embeddedWebM(*document).isEmpty(); }))
			return 35;
		const QUrl url = embeddedWebM(*document);
		QByteArray format;
		if (Log::imageDataFromDataUrl(url, format) != inlineWebMFixture() || format != QByteArray("webm"))
			return 36;
		checkpoint("inline WebM bytes reached real chat document; requesting resource/layout");
		const QVariant resource = document->resource(QTextDocument::ImageResource, url);
		const QImage poster     = resource.value< QImage >();
		std::fprintf(stderr, "%s WebM resource type=%s imageNull=%d\n", qPrintable(role),
					 resource.typeName() ? resource.typeName() : "invalid", poster.isNull());
		std::fflush(stderr);
		(void) document->documentLayout()->documentSize();
#	ifdef USE_CHAT_WEBM
		if (poster.isNull() || !document->isChatVideo(url))
			return 37;
		QSignalSpy frames(document, &LogDocument::animationFrameChanged);
		document->toggleChatVideo(url);
		if (!waitFor([&]() { return frames.size() >= 2; }, 5000))
			return 38;
		document->toggleChatVideo(url);
		checkpoint("real inline WebM playback advanced frames");
#	else
		Q_UNUSED(poster);
		checkpoint("USE_CHAT_WEBM absent: this Windows build relays inline media but has no playback decoder");
#	endif
	}
	maximumGap = qMax(maximumGap, gap.elapsed());
	std::fprintf(stderr, "%s WebM GUI maximum heartbeat gap %lld ms\n", qPrintable(role),
				 static_cast< long long >(maximumGap));
	std::fflush(stderr);
	if (maximumGap > 2000 || heartbeats <= initialHeartbeats)
		return 41;
	if (!writeFixtureFile(shared.filePath(role + ".done"), "done")
		|| !waitFor([&]() { return QFile::exists(shared.filePath(peer + ".done")); }))
		return 42;
	checkpoint("inline WebM drop scenario complete");
	return 0;
}

// Each real client needs its own process: Global, ClientUser and the SQL
// connections are singletons. Only disposable generated identities are accepted
// by the test driver; all production dialog handlers and transports stay wired.
static int tlsChild(const QStringList &arguments) {
	const qsizetype index = arguments.indexOf(QStringLiteral("--tls-child"));
	if (index < 0 || arguments.size() != index + 7)
		return 10;
	const QString role = arguments[index + 1], sharedPath = arguments[index + 2];
	const quint16 port                 = arguments[index + 3].toUShort();
	const QByteArray certificateDigest = arguments[index + 4].toLatin1();
	const QByteArray serverDigest      = QByteArray::fromHex(arguments[index + 5].toLatin1());
	// "first-contact" uses the opt-in safety-number dialogs; "first-use" is the
	// default trust-on-first-use flow, which must not show any dialog.
	const bool firstContact            = arguments[index + 6] == QStringLiteral("first-contact")
										 || arguments[index + 6] == QStringLiteral("webm-oversized");
	const bool firstUse                = arguments[index + 6] == QStringLiteral("first-use");
	const bool inlineWebM              = arguments[index + 6].startsWith(QStringLiteral("webm-"));
	const bool oversizedWebM           = arguments[index + 6] == QStringLiteral("webm-oversized");
	const bool sending                 = role == QStringLiteral("sender");
	const QString peer                 = sending ? QStringLiteral("receiver") : QStringLiteral("sender");
	const QDir shared(sharedPath);
	return mainWindowChild([&](Global &global, MainWindow &window, int &heartbeats) {
		auto *manager = global.fileTransferManager;
		checkpoint("creating disposable identity");
		if (!manager->identity()->createIdentity(QStringLiteral("owned-integration-passphrase")))
			return 11;
		if (!writeFixtureFile(shared.filePath(role + ".fingerprint"), manager->identity()->fingerprint()))
			return 12;
		if (!waitFor([&]() { return QFile::exists(shared.filePath(peer + ".fingerprint")); }))
			return 13;
		const QByteArray peerFingerprint = readFixtureFile(shared.filePath(peer + ".fingerprint"));
		if (peerFingerprint.size() != manager->identity()->fingerprint().size())
			return 14;
		global.s.bFTRequireVerification = firstContact;
		if (!firstContact && !firstUse) {
			if (manager->trustStore()->checkAndPin(serverDigest, peer, peerFingerprint, QString())
					!= PQFT::TrustState::Pinned
				|| !manager->trustStore()->markVerified(serverDigest, peer, peerFingerprint))
				return 15;
		}
		global.db->setDigest(QStringLiteral("127.0.0.1"), port, QString::fromLatin1(certificateDigest));
		global.sh = std::make_shared< ServerHandler >();
		global.sh->moveToThread(global.sh.get());
		window.rtLast           = MumbleProto::Reject_RejectType_None;
		window.bRetryServer     = false;
		global.s.bReconnect     = false;
		const auto disconnect   = qScopeGuard([&]() {
			QObject::disconnect(manager, nullptr, &window, nullptr);
			manager->disconnectCleanup();
			global.sh->disconnect();
			global.sh->wait();
			// PluginManager::on_userRemoved still requires the connection ID.
			checkpoint("clearing disconnected client model");
			if (global.sh->hasSynchronized())
				window.serverDisconnected(QAbstractSocket::UnknownSocketError, QString());
			else
				window.pmModel->removeAll();
			global.sh.reset();
		});
		bool fingerprintMatches = true;
		int contacts = 0, acceptedDialogs = 0, sendDialogs = 0;
		unsigned int peerSession = 0;
		QElapsedTimer contactElapsed;
		const int contactDelay = qBound(0, qEnvironmentVariableIntValue("MUMBLE_TEST_FT_CONTACT_DELAY_MS"), 20000);
		QObject::connect(manager, &FileTransferManager::firstContact, &window,
						 [&](unsigned int, const QByteArray &fingerprint, const QString &) {
							 ++contacts;
							 contactElapsed.start();
							 fingerprintMatches = fingerprintMatches && fingerprint == peerFingerprint;
						 });
		QTimer acceptOwnedDialog;
		acceptOwnedDialog.setInterval(10);
		QObject::connect(&acceptOwnedDialog, &QTimer::timeout, &window, [&]() {
			for (auto *dialog : window.findChildren< FileSendDialog * >()) {
				if (!sending || !dialog->isVisible())
					continue;
				auto *recipients = dialog->findChild< QListWidget * >();
				auto *buttons    = dialog->findChild< QDialogButtonBox * >();
				if (!recipients || !buttons)
					continue;
				for (int row = 0; row < recipients->count(); ++row) {
					auto *item = recipients->item(row);
					item->setCheckState(item->data(Qt::UserRole).toUInt() == peerSession ? Qt::Checked : Qt::Unchecked);
				}
				if (buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
					++sendDialogs;
					buttons->button(QDialogButtonBox::Ok)->click();
				}
			}
			if (!firstContact || !fingerprintMatches || !contactElapsed.isValid()
				|| contactElapsed.elapsed() < contactDelay)
				return;
			for (auto *dialog : window.findChildren< SafetyNumberDialog * >()) {
				if (!dialog->isVisible())
					continue;
				if (auto *buttons = dialog->findChild< QDialogButtonBox * >()) {
					++acceptedDialogs;
					buttons->button(QDialogButtonBox::Yes)->click();
				}
			}
		});
		acceptOwnedDialog.start();
		checkpoint("connecting real ServerHandler");
		global.sh->setConnectionInfo(QStringLiteral("127.0.0.1"), port, role, QString());
		global.sh->start();
		if (!waitFor([&]() { return global.uiSession != 0 && global.sh->hasSynchronized(); }))
			return 16;
		checkpoint("client synchronized");
		if (!waitFor([&]() {
				for (const auto *user : ClientUser::c_qmUsers) {
					if (user->qsName == peer && user->bFileTransferCapable && user->bFileTransferDeliveryReceipts) {
						peerSession = user->uiSession;
						return true;
					}
				}
				return false;
			}))
			return 17;
		manager->refreshPinCache();
		checkpoint("peer capability observed");
		if (oversizedWebM && !waitFor([&]() { return global.uiImageLength == 1024u; }))
			return 30;
		if (inlineWebM && !oversizedWebM)
			return inlineWebMDrop(global, window, shared, sending, heartbeats);
		bool complete = false, failed = false;
		int previousState = -1;
		QByteArray transferId;
		QObject::connect(
			manager, &FileTransferManager::transferUpdated, &window, [&](const PQFT::FTTransferInfo &info) {
				transferId = info.transferId;
				if (previousState != static_cast< int >(info.state)) {
					previousState = static_cast< int >(info.state);
					std::fprintf(stderr, "%s transfer state %d\n", qPrintable(role), previousState);
					std::fflush(stderr);
				}
				if (!info.error.isEmpty()) {
					checkpoint(qPrintable(info.error));
					failed = true;
				}
				if (!sending && info.state == PQFT::FTTransferInfo::State::Ready)
					manager->saveTransferAs(info.transferId, shared.filePath(QStringLiteral("received.bin")));
				if (info.state == PQFT::FTTransferInfo::State::Saved)
					complete = true;
			});
		if (!writeFixtureFile(shared.filePath(role + ".ready"), "ready"))
			return 18;
		QElapsedTimer heartbeatGap;
		heartbeatGap.start();
		qint64 maximumHeartbeatGap = 0;
		QTimer responsiveness;
		responsiveness.setInterval(10);
		QObject::connect(&responsiveness, &QTimer::timeout,
						 [&]() { maximumHeartbeatGap = qMax(maximumHeartbeatGap, heartbeatGap.restart()); });
		responsiveness.start();
		if (sending) {
			if (!waitFor([&]() { return QFile::exists(shared.filePath(peer + ".ready")); }))
				return 19;
			const auto recipient = manager->sendRecipient(peerSession);
			if (!recipient)
				return 20;
			checkpoint("opening real MainWindow send flow");
			if (oversizedWebM) {
				QSignalSpy sentInline(window.qteChat, &ChatbarTextEdit::pastedImage);
				QSignalSpy files(window.qteChat, &ChatbarTextEdit::fileDropRequested);
				dropLocalWebM(window, shared.filePath(QStringLiteral("source.webm")));
				if (sentInline.size() != 0 || files.size() != 1)
					return 43;
			} else {
				window.startFileSend(shared.filePath(QStringLiteral("source.bin")));
			}
			if (sendDialogs != 1)
				return 21;
		}
		const int startHeartbeats = heartbeats;
		if (!waitFor([&]() { return complete || failed; }, 45000) || failed || !complete)
			return 22;
		maximumHeartbeatGap = qMax(maximumHeartbeatGap, heartbeatGap.elapsed());
		std::fprintf(stderr, "%s maximum GUI heartbeat gap %lld ms\n", qPrintable(role),
					 static_cast< long long >(maximumHeartbeatGap));
		std::fflush(stderr);
		if (heartbeats <= startHeartbeats || maximumHeartbeatGap > 2000 || !fingerprintMatches
			|| (firstContact && (contacts == 0 || acceptedDialogs == 0))
			|| (!firstContact && (contacts != 0 || acceptedDialogs != 0)))
			return 23;
		if (firstUse) {
			// Both sides remembered the other's key on first use, unverified.
			PQFT::PinnedPeer pin;
			if (!manager->trustStore()->lookup(pin, serverDigest, peer) || pin.fingerprint != peerFingerprint
				|| pin.verified || !window.findChildren< SafetyNumberDialog * >().isEmpty())
				return 26;
		}
		const QUrl card(
			QStringLiteral("data:application/mumble-file;base64,%1").arg(QString::fromLatin1(transferId.toBase64())));
		if (window.qteLog->document()->resource(QTextDocument::ImageResource, card).value< QImage >().isNull()
			|| !window.qteLog->document()->toHtml().contains(QString::fromLatin1(transferId.toBase64())))
			return 24;
		if (!writeFixtureFile(shared.filePath(role + ".done"), "done")
			|| !waitFor([&]() { return QFile::exists(shared.filePath(peer + ".done")); }))
			return 25;
		checkpoint("real transfer saved and acknowledged");
		return 0;
	});
}
#endif

class TestFileTransferMainWindow : public QObject {
	Q_OBJECT
private slots:
#ifdef MUMBLE_TEST_MURMUR_BINARY
	void realClientTransfer_data() {
		QTest::addColumn< QString >("mode");
		QTest::newRow("preverified") << QStringLiteral("preverified");
		QTest::newRow("first-contact-dialogs") << QStringLiteral("first-contact");
		QTest::newRow("first-use-without-dialogs") << QStringLiteral("first-use");
	}

	void realClientTransfer() {
		QFETCH(QString, mode);
		runClients(mode, false, false);
	}
	void inlineWebMDrop_data() {
		QTest::addColumn< bool >("oversized");
		QTest::newRow("accepted-inline-video") << false;
		QTest::newRow("oversized-video-file-transfer-fallback") << true;
	}
	void inlineWebMDrop() {
		QFETCH(bool, oversized);
		runClients(QStringLiteral("preverified"), true, oversized);
	}

private:
	void runClients(const QString &mode, bool webm, bool oversized) {
		const QString logs = QDir::current().filePath(QStringLiteral("test-results/file-transfer-main-window"));
		QVERIFY(QDir().mkpath(logs));
		const QString suffix =
			webm ? (oversized ? QStringLiteral("-webm-oversized.log") : QStringLiteral("-webm-small.log"))
				 : QStringLiteral("-%1.log").arg(mode);
		// A failed server launch must not leave an earlier run's client logs
		// looking like evidence from this attempt.
		for (const QString &role : { QStringLiteral("sender"), QStringLiteral("receiver"), QStringLiteral("server") })
			QVERIFY(writeFixtureFile(QDir(logs).filePath(role + suffix), "Fixture startup pending\n"));
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QTcpServer reservation;
		QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
		const quint16 port = reservation.serverPort();
		reservation.close();
		const QString iniPath = directory.filePath(QStringLiteral("server.ini"));
		QVERIFY(writeFixtureFile(
			iniPath,
			QStringLiteral(
				"host=127.0.0.1\nport=%1\ndatabase=%2\nbonjour=false\nautobanAttempts=0\nimagemessagelength=%3\n")
				.arg(port)
				.arg(directory.filePath(QStringLiteral("server.sqlite")))
				.arg(webm && oversized ? 1024 : 131072)
				.toUtf8()));
		QByteArray source(600123, Qt::Uninitialized);
		for (qsizetype index = 0; index < source.size(); ++index)
			source[index] = static_cast< char >((index * 31 + 7) & 255);
		if (webm)
			source = inlineWebMFixture();
		QVERIFY(writeFixtureFile(directory.filePath(QStringLiteral("source.bin")), source));
		if (webm)
			QVERIFY(writeFixtureFile(directory.filePath(QStringLiteral("source.webm")), inlineWebMFixture()));
		QProcess server;
		server.setProcessChannelMode(QProcess::MergedChannels);
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.remove(QStringLiteral("QT_QPA_PLATFORM"));
		server.setProcessEnvironment(environment);
		server.start(QStringLiteral(MUMBLE_TEST_MURMUR_BINARY),
					 { QStringLiteral("--foreground"), QStringLiteral("--ini"), iniPath });
		const auto stopServer    = qScopeGuard([&]() {
			if (server.state() == QProcess::NotRunning)
				return;
			server.terminate();
			if (!server.waitForFinished(3000)) {
				server.kill();
				server.waitForFinished(5000);
			}
		});
		const auto serverFailure = [&](const QString &reason) {
			const QByteArray output =
				reason.toUtf8() + "\n" + server.errorString().toUtf8() + "\n" + server.readAllStandardOutput();
			(void) writeFixtureFile(QDir(logs).filePath("server" + suffix), output);
			return output;
		};
		if (!server.waitForStarted(5000)) {
			const QByteArray output = serverFailure(QStringLiteral("Owned server did not start"));
			QFAIL(output.constData());
		}
		QTcpSocket startupProbe;
		bool listening             = false;
		const bool startupFinished = waitFor(
			[&]() {
				if (server.state() == QProcess::NotRunning)
					return true;
				startupProbe.abort();
				startupProbe.connectToHost(QHostAddress::LocalHost, port);
				listening = startupProbe.waitForConnected(100);
				return listening;
			},
			60000);
		startupProbe.abort();
		if (!startupFinished || !listening) {
			const QByteArray output =
				serverFailure(QStringLiteral("Owned server exited or never listened; exit %1").arg(server.exitCode()));
			QFAIL(output.constData());
		}
		QSslSocket probe;
		probe.setPeerVerifyMode(QSslSocket::VerifyNone);
		probe.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);
		if (!probe.waitForEncrypted(15000)) {
			const QByteArray output =
				serverFailure(QStringLiteral("Owned TLS probe failed: %1").arg(probe.errorString()));
			QFAIL(output.constData());
		}
		const QByteArray certificateDigest = probe.peerCertificate().digest(QCryptographicHash::Sha1).toHex();
		// This is the real ServerHandler trust namespace, distinct from the
		// certificate digest used to accept this owned TLS server in the DB.
		const QByteArray serverDigest =
			QCryptographicHash::hash(probe.peerCertificate().publicKey().toDer(), QCryptographicHash::Sha1).toHex();
		probe.abort();
		QProcess sender, receiver;
		const auto stopChildren = qScopeGuard([&]() {
			for (auto *child : { &sender, &receiver }) {
				if (child->state() != QProcess::NotRunning) {
					child->kill();
					child->waitForFinished(5000);
				}
			}
		});
		for (auto *child : { &sender, &receiver }) {
			child->setProcessChannelMode(QProcess::MergedChannels);
			child->start(
				QCoreApplication::applicationFilePath(),
				{ QStringLiteral("-platform"), QGuiApplication::platformName(), QStringLiteral("--tls-child"),
				  child == &sender ? QStringLiteral("sender") : QStringLiteral("receiver"), directory.path(),
				  QString::number(port), QString::fromLatin1(certificateDigest), QString::fromLatin1(serverDigest),
				  webm ? (oversized ? QStringLiteral("webm-oversized") : QStringLiteral("webm-small")) : mode });
			QVERIFY(child->waitForStarted(5000));
		}
		const bool finished = waitFor(
			[&]() { return sender.state() == QProcess::NotRunning && receiver.state() == QProcess::NotRunning; },
			75000);
		if (!finished) {
			sender.kill();
			receiver.kill();
			sender.waitForFinished(5000);
			receiver.waitForFinished(5000);
		}
		const QByteArray senderOutput   = sender.readAllStandardOutput(),
						 receiverOutput = receiver.readAllStandardOutput(),
						 serverOutput   = server.readAllStandardOutput();
		QVERIFY(writeFixtureFile(QDir(logs).filePath("sender" + suffix), senderOutput));
		QVERIFY(writeFixtureFile(QDir(logs).filePath("receiver" + suffix), receiverOutput));
		QVERIFY(writeFixtureFile(QDir(logs).filePath("server" + suffix), serverOutput));
		const QByteArray diagnostics =
			QStringLiteral("sender exit %1 status %2; receiver exit %3 status %4\nFull logs: %5\n")
				.arg(sender.exitCode())
				.arg(sender.exitStatus())
				.arg(receiver.exitCode())
				.arg(receiver.exitStatus())
				.arg(logs)
				.toUtf8()
			+ "SENDER TAIL:\n" + senderOutput.right(1400) + "\nRECEIVER TAIL:\n" + receiverOutput.right(1400);
		QVERIFY2(finished, diagnostics.constData());
		QVERIFY2(sender.exitStatus() == QProcess::NormalExit && sender.exitCode() == 0, diagnostics.constData());
		QVERIFY2(receiver.exitStatus() == QProcess::NormalExit && receiver.exitCode() == 0, diagnostics.constData());
		if (!webm || oversized)
			QCOMPARE(QCryptographicHash::hash(readFixtureFile(directory.filePath(QStringLiteral("received.bin"))),
											  QCryptographicHash::Sha256),
					 QCryptographicHash::hash(source, QCryptographicHash::Sha256));
	}
#endif
private slots:
	void transferUpdatesKeepGuiResponsive() {
		QProcess child;
		child.setProcessChannelMode(QProcess::MergedChannels);
		child.start(
			QCoreApplication::applicationFilePath(),
			{ QStringLiteral("-platform"), QGuiApplication::platformName(), QStringLiteral("--main-window-child") });
		QVERIFY(child.waitForStarted(5000));
		const bool finished = child.waitForFinished(20000);
		if (!finished) {
			child.kill();
			child.waitForFinished(5000);
		}
		const QByteArray diagnostics = child.readAllStandardOutput();
		QVERIFY2(finished,
				 qPrintable(QStringLiteral("Actual MainWindow file-transfer update blocked for twenty seconds:\n%1")
								.arg(QString::fromLocal8Bit(diagnostics))));
		QVERIFY2(child.exitStatus() == QProcess::NormalExit, diagnostics.constData());
		QVERIFY2(child.exitCode() == 0, diagnostics.constData());
	}
};

int main(int argc, char **argv) {
	MumbleApplication application(argc, argv);
#ifdef MUMBLE_TEST_MURMUR_BINARY
	MumbleSSL::initialize();
	if (application.arguments().contains(QStringLiteral("--tls-child"))) {
		const int result = tlsChild(application.arguments());
		std::fprintf(stderr, "TLS child exit %d\n", result);
		std::fflush(stderr);
		Global::g_global_struct = nullptr;
		return result;
	}
#endif
	if (application.arguments().contains(QStringLiteral("--main-window-child"))) {
		const int result        = mainWindowChild();
		Global::g_global_struct = nullptr;
		return result;
	}
	TestFileTransferMainWindow test;
	return QTest::qExec(&test, argc, argv);
}

#include "TestFileTransferMainWindow.moc"
