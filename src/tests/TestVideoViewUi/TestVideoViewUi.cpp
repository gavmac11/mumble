// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoViewUi.h"

#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

class TestVideoViewUi : public QObject {
	Q_OBJECT

private slots:
	void fullscreenTracksExternalChanges() {
		QDialog window;
		auto *button = VideoViewUi::fullscreenButton(&window);
		window.showMaximized();
		QCoreApplication::processEvents();
		button->click();
		QVERIFY(window.isFullScreen());
		QCOMPARE(button->text(), QStringLiteral("Exit full screen"));

		// Simulate a native window-manager exit, without using our toggle.
		window.setWindowState(window.windowState() & ~Qt::WindowFullScreen);
		QVERIFY(!window.isFullScreen());
		QCOMPARE(button->text(), QStringLiteral("Full screen"));
		QVERIFY(window.isMaximized());

		window.setWindowState(window.windowState() | Qt::WindowFullScreen);
		QCOMPARE(button->text(), QStringLiteral("Exit full screen"));
		button->click();
		QVERIFY(!window.isFullScreen());
		QVERIFY(window.isMaximized());
		QCOMPARE(button->text(), QStringLiteral("Full screen"));
	}

	void fullscreenKeyboard() {
		QDialog window;
		VideoViewUi::fullscreenButton(&window);
		window.show();
		window.activateWindow();
		QTRY_VERIFY(window.isActiveWindow());
		QTest::keySequence(&window, QKeySequence(QKeySequence::FullScreen));
		QTRY_VERIFY(window.isFullScreen());
		QTest::keyClick(&window, Qt::Key_Escape);
		QTRY_VERIFY(!window.isFullScreen());
		QVERIFY(window.isVisible());
		QTest::keyClick(&window, Qt::Key_Escape);
		QTRY_VERIFY(!window.isVisible());
	}

	void respectsThemeChanges() {
		QDialog window;
		VideoViewUi::style(&window);
		auto *heading = new QLabel(QStringLiteral("Shared video"), &window);
		heading->setObjectName(QStringLiteral("videoHeading"));
		auto *layout = new QVBoxLayout(&window);
		layout->addWidget(heading);
		window.show();

		qApp->setStyleSheet(QStringLiteral("QWidget { background: #ffffff; color: #111111; font-size: 24px; }"));
		QCoreApplication::processEvents();
		QCOMPARE(window.palette().color(QPalette::Window), QColor("#ffffff"));
		QCOMPARE(heading->palette().color(QPalette::WindowText), QColor("#111111"));
		QCOMPARE(heading->font().pixelSize(), 24);

		// A theme or accessibility-font change must also update already-open video windows.
		qApp->setStyleSheet(QStringLiteral("QWidget { background: #111111; color: #ffff00; font-size: 32px; }"));
		QCoreApplication::processEvents();
		QCOMPARE(window.palette().color(QPalette::Window), QColor("#111111"));
		QCOMPARE(heading->palette().color(QPalette::WindowText), QColor("#ffff00"));
		QCOMPARE(heading->font().pixelSize(), 32);
	}

	void cleanup() { qApp->setStyleSheet(QString()); }
};

QTEST_MAIN(TestVideoViewUi)
#include "TestVideoViewUi.moc"
