// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "Logger.h"

#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QTest>

static int previousMessages = 0;
static int laterMessages    = 0;
static void previousHandler(QtMsgType, const QMessageLogContext &, const QString &) {
	++previousMessages;
}
static void laterHandler(QtMsgType, const QMessageLogContext &, const QString &) {
	++laterMessages;
}

static int lateLogChild(const QString &mode) {
	qInstallMessageHandler(previousHandler);
	mumble::log::init();
	if (mode == "repeated")
		mumble::log::init();
	if (mode == "later-owner")
		qInstallMessageHandler(laterHandler);
	mumble::log::restoreQtMessageHandler();
	// Repeated cleanup must not replace a later owner's handler either.
	mumble::log::restoreQtMessageHandler();
	spdlog::shutdown();
	// This used to dereference spdlog's destroyed default logger during Qt plugin unload.
	qInfo("Private late Qt teardown message");
	return mode == "later-owner" ? (laterMessages == 1 && previousMessages == 0 ? 0 : 2)
								 : (previousMessages == 1 && laterMessages == 0 ? 0 : 3);
}

class TestQtLogger : public QObject {
	Q_OBJECT
private slots:
	void lateQtLog_data() {
		QTest::addColumn< QString >("mode");
		QTest::newRow("prior-handler") << QString("normal");
		QTest::newRow("repeated-init") << QString("repeated");
		QTest::newRow("later-handler-owner") << QString("later-owner");
	}
	void lateQtLog() {
		QFETCH(QString, mode);
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(), { "--late-log-child", mode });
		QVERIFY(child.waitForStarted());
		QVERIFY(child.waitForFinished(10000));
		QCOMPARE(child.exitStatus(), QProcess::NormalExit);
		QCOMPARE(child.exitCode(), 0);
	}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().size() == 3 && app.arguments().at(1) == "--late-log-child")
		return lateLogChild(app.arguments().at(2));
	TestQtLogger test;
	return QTest::qExec(&test, argc, argv);
}
#include "TestQtLogger.moc"
