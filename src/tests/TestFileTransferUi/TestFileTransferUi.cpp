// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "JSONSerialization.h"
#include "Log.h"
#include "PQFileTransfer/engine/FTCardRender.h"
#include "PQFileTransfer/engine/FTSavePolicy.h"
#include "PQFileTransfer/identity/FTIdentity.h"
#include "PQFileTransfer/widgets/FileTransferDialogs.h"
#include "Settings.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

class TestLogSchemes : public Log {
public:
	using Log::allowedSchemes;
};

class TestFileTransferUi : public QObject {
	Q_OBJECT
private slots:
	void automaticNames_data() {
		QTest::addColumn< QString >("name");
		QTest::addColumn< bool >("allowed");
		int row = 0;
		for (const QString &name : QStringList{ QString(),
												QStringLiteral(".zprofile"),
												QStringLiteral(".bash_login"),
												QStringLiteral(".hidden"),
												QStringLiteral(".."),
												QStringLiteral("."),
												QStringLiteral("../escape"),
												QStringLiteral("folder/file"),
												QStringLiteral("folder\\file"),
												QStringLiteral("name:stream"),
												QStringLiteral("C:relative"),
												QStringLiteral("NUL"),
												QStringLiteral("nul.txt"),
												QStringLiteral("CON.log"),
												QStringLiteral("PRN"),
												QStringLiteral("aux.dat"),
												QStringLiteral("COM1.txt"),
												QStringLiteral("com9"),
												QStringLiteral("LPT1"),
												QStringLiteral("lpt9.pdf"),
												QStringLiteral("COM0"),
												QStringLiteral("CLOCK$"),
												QStringLiteral("CONIN$"),
												QStringLiteral("CONOUT$.txt"),
												QStringLiteral("COM\u00b9.txt"),
												QStringLiteral("LPT\u00b2"),
												QStringLiteral("COM\u00b3"),
												QStringLiteral("NUL .txt"),
												QStringLiteral("\U0001f600.txt"),
												QStringLiteral("\u00a0leading"),
												QStringLiteral("trailing\u3000"),
												QStringLiteral("\u200bleading"),
												QStringLiteral("\ufeffleading"),
												QStringLiteral("com\u00b9.txt"),
												QStringLiteral("lpt\u00b3.log"),
												QStringLiteral("name. "),
												QStringLiteral("AUX"),
												QStringLiteral("CON"),
												QStringLiteral("trailing."),
												QStringLiteral("trailing "),
												QStringLiteral(" leading"),
												QStringLiteral("bad\nname"),
												QStringLiteral("bad\tname"),
												QStringLiteral("bad\u007fname"),
												QStringLiteral("bad\u202ename"),
												QStringLiteral("bad\u2028name"),
												QStringLiteral("bad?name"),
												QStringLiteral("bad*name"),
												QStringLiteral("bad<name"),
												QStringLiteral("bad>name"),
												QStringLiteral("bad|name"),
												QStringLiteral("bad\"name"),
												QString(256, QLatin1Char('x')),
												QString(128, QChar(0x00e9)),
												QStringLiteral("bad") + QChar(0) + QStringLiteral("name"),
												QString(QChar(0xd800)) }) {
			const QByteArray tag = "rejected-" + QByteArray::number(row++);
			QTest::newRow(tag.constData()) << name << false;
		}
		for (const QString &name :
			 { QStringLiteral("report.pdf"), QStringLiteral("family photo.jpg"), QStringLiteral("r\u00e9sum\u00e9.txt"),
			   QStringLiteral("\u6587\u4ef6.txt"), QStringLiteral("COM10.txt"), QStringLiteral("null.txt"),
			   QStringLiteral("archive.tar.gz"), QString(255, QLatin1Char('x')) }) {
			const QByteArray tag = "accepted-" + QByteArray::number(row++);
			QTest::newRow(tag.constData()) << name << true;
		}
	}
	void automaticNames() {
		QFETCH(QString, name);
		QFETCH(bool, allowed);
		QCOMPARE(PQFT::isSafeAutomaticFileName(name), allowed);
	}
	void automaticSaveEligibility() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		Settings settings;
		settings.bFTAutoAcceptPinned = true;
		settings.qsFTDownloadDir     = dir.path();
		PQFT::FTTransferInfo info;
		info.incoming = true;
		info.state    = PQFT::FTTransferInfo::State::Ready;
		info.fileName = QStringLiteral("report.pdf");
		QCOMPARE(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified),
				 QDir(dir.path()).filePath(info.fileName));
		for (auto trust : { PQFT::TrustState::NewPeer, PQFT::TrustState::Pinned, PQFT::TrustState::Changed })
			QVERIFY(PQFT::automaticSaveTarget(settings, info, trust).isEmpty());
		info.incoming = false;
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		info.incoming = true;
		info.state    = PQFT::FTTransferInfo::State::Transferring;
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		info.state = PQFT::FTTransferInfo::State::Ready;
		info.error = QStringLiteral("retry manually");
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		info.error.clear();
		info.fileName = QStringLiteral(".zprofile");
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		info.fileName                = QStringLiteral("report.pdf");
		settings.bFTAutoAcceptPinned = false;
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		settings.bFTAutoAcceptPinned = true;
		settings.qsFTDownloadDir.clear();
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
		settings.qsFTDownloadDir = QStringLiteral("relative-folder");
		QVERIFY(PQFT::automaticSaveTarget(settings, info, PQFT::TrustState::Verified).isEmpty());
	}
	void manualHistoryDoesNotRedirectAutomaticSaving() {
		QTemporaryDir automaticDir, manualDir;
		QVERIFY(automaticDir.isValid());
		QVERIFY(manualDir.isValid());
		Settings settings;
		settings.bFTAutoAcceptPinned = true;
		settings.qsFTDownloadDir     = automaticDir.path();
		QCOMPARE(PQFT::manualSaveDirectory(settings), automaticDir.path());
		PQFT::rememberManualSaveDirectory(settings, QDir(manualDir.path()).filePath(QStringLiteral("chosen.txt")));
		const nlohmann::json serialized = settings;
		const Settings restored         = serialized.get< Settings >();
		QCOMPARE(restored.qsFTDownloadDir, automaticDir.path());
		QCOMPARE(PQFT::manualSaveDirectory(restored), manualDir.path());
		PQFT::FTTransferInfo info;
		info.incoming = true;
		info.state    = PQFT::FTTransferInfo::State::Ready;
		info.fileName = QStringLiteral("next.txt");
		QCOMPARE(PQFT::automaticSaveTarget(restored, info, PQFT::TrustState::Verified),
				 QDir(automaticDir.path()).filePath(info.fileName));
		// Existing profiles have no manual-history key and retain their configured folder.
		auto legacy = serialized;
		legacy["misc"].erase("file_transfer_manual_save_dir");
		const Settings restoredLegacy = legacy.get< Settings >();
		QVERIFY(restoredLegacy.qsFTManualSaveDir.isEmpty());
		QCOMPARE(PQFT::manualSaveDirectory(restoredLegacy), automaticDir.path());
	}
	void olderAutomaticFolderRequiresReview_data() {
		QTest::addColumn< QByteArray >("policyJson");
		QTest::newRow("missing") << QByteArray();
		QTest::newRow("old") << QByteArray("0");
		QTest::newRow("unknown-future") << QByteArray("2");
		QTest::newRow("string") << QByteArray("\"1\"");
		QTest::newRow("boolean") << QByteArray("true");
	}
	void olderAutomaticFolderRequiresReview() {
		QFETCH(QByteArray, policyJson);
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		Settings settings;
		settings.qsFTDownloadDir     = dir.path();
		settings.bFTAutoAcceptPinned = true;
		nlohmann::json legacy        = settings;
		if (policyJson.isEmpty())
			legacy["misc"].erase("file_transfer_auto_save_policy_version");
		else
			legacy["misc"]["file_transfer_auto_save_policy_version"] = nlohmann::json::parse(policyJson.constData());
		QTest::ignoreMessage(QtWarningMsg, "Automatic file saving was disabled for an older profile. Review the "
										   "download directory and enable it again in File Transfer settings.");
		Settings restored = legacy.get< Settings >();
		QVERIFY(!restored.bFTAutoAcceptPinned);
		QCOMPARE(restored.qsFTDownloadDir, dir.path());
		PQFT::FTTransferInfo info;
		info.incoming = true;
		info.state    = PQFT::FTTransferInfo::State::Ready;
		info.fileName = QStringLiteral("next.txt");
		QVERIFY(PQFT::automaticSaveTarget(restored, info, PQFT::TrustState::Verified).isEmpty());
		// Saving the migrated profile does not silently enable automatic saving.
		const nlohmann::json migrated = restored;
		QVERIFY(migrated["misc"]["file_transfer_auto_save_policy_version"] == 1);
		QVERIFY(!migrated.get< Settings >().bFTAutoAcceptPinned);
		// Explicitly enabling the feature after reviewing the folder persists normally.
		restored.bFTAutoAcceptPinned   = true;
		const nlohmann::json confirmed = restored;
		const Settings confirmedAgain  = confirmed.get< Settings >();
		QVERIFY(confirmedAgain.bFTAutoAcceptPinned);
		QCOMPARE(PQFT::automaticSaveTarget(confirmedAgain, info, PQFT::TrustState::Verified),
				 QDir(dir.path()).filePath(info.fileName));
	}
	void qrContrast_data() {
		QTest::addColumn< bool >("dark");
		QTest::newRow("light") << false;
		QTest::newRow("dark") << true;
	}
	void qrContrast() {
		QFETCH(bool, dark);
		const auto original = QApplication::palette();
		QPalette palette    = original;
		palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
		palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
		QApplication::setPalette(palette);
		// Canonical safety CBOR includes zero/high bytes and must survive QR encoding intact.
		const QByteArray payload = PQFT::safetyQrPayload(QByteArray(48, '\0'), QByteArray(48, '\xff'));
		SafetyNumberDialog dialog("Private fixture", "00141 99734", payload);
		QApplication::setPalette(original);
		QImage qr;
		for (auto *label : dialog.findChildren< QLabel * >()) {
			if (!label->pixmap().isNull())
				qr = label->pixmap().toImage();
		}
		QVERIFY(!qr.isNull());
		// Optional exports let an independent scanner verify the real dialog bitmap.
		const QString exportDir = qEnvironmentVariable("MUMBLE_QR_FIXTURE_EXPORT");
		if (!exportDir.isEmpty()) {
			QVERIFY(QDir().mkpath(exportDir));
			const QString prefix = exportDir + "/" + QString::fromLatin1(QTest::currentDataTag());
			QVERIFY(qr.save(prefix + ".png"));
			QFile expected(prefix + ".bin");
			QVERIFY(expected.open(QIODevice::WriteOnly));
			QCOMPARE(expected.write(payload), payload.size());
		}
		QCOMPARE(qr.pixelColor(0, 0), QColor(Qt::white));
		int black = 0, white = 0;
		for (int y = 0; y < qr.height(); ++y) {
			for (int x = 0; x < qr.width(); ++x) {
				const auto color = qr.pixelColor(x, y);
				black += color == QColor(Qt::black);
				white += color == QColor(Qt::white);
			}
		}
		QVERIFY(black > qr.width());
		QVERIFY(white > qr.width());
	}
	void fileActionByKeyboard() {
		QVERIFY(TestLogSchemes::allowedSchemes().contains(QStringLiteral("mumble-file")));
		const QByteArray id(16, 'x');
		QTextBrowser browser;
		browser.setOpenLinks(false);
		browser.setHtml(Log::fileCardToHtml(id));
		QSignalSpy clicked(&browser, &QTextBrowser::anchorClicked);
		browser.show();
		browser.setFocus();
		QTest::keyClick(&browser, Qt::Key_Tab);
		QTest::keyClick(&browser, Qt::Key_Return);
		QCOMPARE(clicked.size(), 1);
		bool ok = false;
		QCOMPARE(PQFT::fileCardActionId(clicked.at(0).at(0).toUrl(), ok), id);
		QVERIFY(ok);
	}
	void invalidActionUrls() {
		for (const auto &url : { QString("https://example.com/78787878787878787878787878787878"),
								 QString("mumble-file://external/78787878787878787878787878787878"),
								 QString("mumble-file:/78787878787878787878787878787878?extra=1"),
								 QString("mumble-file:/78787878787878787878787878787878#extra"),
								 QString("mumble-file:/not-a-transfer"), QString("mumble-file:/7878") }) {
			bool ok = true;
			QVERIFY(PQFT::fileCardActionId(QUrl(url), ok).isEmpty());
			QVERIFY(!ok);
		}
	}
	void unrelatedDataIsNotAFileAction() {
		bool ok                  = false;
		const QByteArray encoded = QByteArray(16, 'x').toBase64();
		PQFT::fileCardTransferId(QUrl("data:text/plain;base64," + QString::fromLatin1(encoded)), ok);
		QVERIFY(!ok);
		PQFT::fileCardTransferId(QUrl("data:application/mumble-file;base64,eA=="), ok);
		QVERIFY(!ok);
	}
};
QTEST_MAIN(TestFileTransferUi)
#include "TestFileTransferUi.moc"
