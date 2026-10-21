// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatWindow.h"

#include <QtTest/QtTest>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

class TestChatWindow : public QObject {
	Q_OBJECT

private slots:
	void escapePreservesChat() {
		ChatWindow window;
		QVBoxLayout layout(&window);
		QTextEdit input;
		layout.addWidget(&input);
		QSignalSpy finished(&window, &QDialog::finished);
		window.show();
		input.setFocus();
		QTest::keyClicks(&input, "unsent draft");

		// Exercise the propagation from the input, as well as the window itself.
		QTest::keyClick(&input, Qt::Key_Escape);
		QTest::keyClick(&window, Qt::Key_Escape);
		QVERIFY(window.isVisible());
		QCOMPARE(finished.count(), 0);
		QCOMPARE(input.toPlainText(), QStringLiteral("unsent draft"));

		QTest::keyClicks(&input, " continued");
		QCOMPARE(input.toPlainText(), QStringLiteral("unsent draft continued"));
	}

	void explicitCloseNotifiesRestore() {
		ChatWindow window;
		QSignalSpy finished(&window, &QDialog::finished);
		window.show();
		const bool closed = window.close();
		QVERIFY(closed);
		QVERIFY(!window.isVisible());
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(0).toInt(), static_cast< int >(QDialog::Rejected));
	}
};

QTEST_MAIN(TestChatWindow)
#include "TestChatWindow.moc"
