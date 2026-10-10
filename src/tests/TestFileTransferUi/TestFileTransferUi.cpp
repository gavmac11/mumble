// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "Log.h"
#include "PQFileTransfer/engine/FTCardRender.h"
#include "PQFileTransfer/widgets/FileTransferDialogs.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QTextBrowser>

class TestLogSchemes : public Log {
public:
	using Log::allowedSchemes;
};

class TestFileTransferUi : public QObject {
	Q_OBJECT
private slots:
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
		SafetyNumberDialog dialog("Private fixture", "00141 99734", "mumble-fixture-safety-number");
		QApplication::setPalette(original);
		QImage qr;
		for (auto *label : dialog.findChildren< QLabel * >()) {
			if (!label->pixmap().isNull())
				qr = label->pixmap().toImage();
		}
		QVERIFY(!qr.isNull());
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
