// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "Logger.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>

static QString fatalCleanupMarker;
static void unsafeFatalCleanup() {
	QFile marker(fatalCleanupMarker);
	if (marker.open(QIODevice::WriteOnly)) {
		marker.write("static cleanup ran");
		marker.close();
	}
	spdlog::shutdown();
	qInfo("Private Qt message during fatal static cleanup");
}

static int fatalLogChild(const QString &marker, const QString &mode) {
	fatalCleanupMarker = marker;
	mumble::log::init();
	std::atexit(unsafeFatalCleanup);
	if (mode == "qt")
		qFatal("Private expected startup failure");
	else
		mumble::log::fatal("Private expected startup failure");
	return 2;
}

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
	void fatalLogDoesNotRunStaticCleanup_data() {
		QTest::addColumn< QString >("mode");
		QTest::newRow("qt-fatal-message") << QString("qt");
		QTest::newRow("direct-fatal-call") << QString("direct");
	}
	void fatalLogDoesNotRunStaticCleanup() {
		QFETCH(QString, mode);
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const QString marker = root.filePath("fatal-static-cleanup.txt");
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(), { "--fatal-log-child", marker, mode });
		QVERIFY(child.waitForStarted());
		QVERIFY(child.waitForFinished(10000));
		QCOMPARE(child.exitStatus(), QProcess::NormalExit);
		QCOMPARE(child.exitCode(), 1);
		QVERIFY(child.readAllStandardOutput().contains("Private expected startup failure"));
		QVERIFY(!QFile::exists(marker));
	}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().size() == 3 && app.arguments().at(1) == "--late-log-child")
		return lateLogChild(app.arguments().at(2));
	if (app.arguments().size() == 4 && app.arguments().at(1) == "--fatal-log-child")
		return fatalLogChild(app.arguments().at(2), app.arguments().at(3));
	TestQtLogger test;
	return QTest::qExec(&test, argc, argv);
}
#include "TestQtLogger.moc"
