// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "Database.h"
#include "Logger.h"
#include "Global.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>

static bool seedDatabase(const QString &path) {
	bool success = false;
	{
		QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "private-seed");
		db.setDatabaseName(path);
		if (db.open()) {
			QSqlQuery query(db);
			success = query.exec("CREATE TABLE sentinel (value TEXT)")
					  && query.exec("INSERT INTO sentinel VALUES ('private fixture')");
			db.close();
		}
	}
	QSqlDatabase::removeDatabase("private-seed");
	return success;
}

static QByteArray fileHash(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static int databaseChild(const QString &root, const QString &mode) {
	// An explicit config base and an existing local fallback keep even the negative
	// control out of the real user's data directory.
	Global global(QDir(root).filePath("private-settings.json"));
	Global::g_global_struct = &global;
	const QString expected =
		QDir(root).filePath(mode == "default" ? QStringLiteral("mumble.sqlite") : QStringLiteral("explicit.sqlite"));
	const bool directory        = mode.startsWith("directory");
	global.s.qsDatabaseLocation = mode == "default" ? QString() : (directory ? root : expected);
	if (mode == "directory-app-logger")
		mumble::log::init();
	if (directory) {
		QTimer::singleShot(0, [&root]() {
			auto *dialog = qobject_cast< QMessageBox * >(QApplication::activeModalWidget());
			if (dialog && dialog->textFormat() == Qt::PlainText && dialog->text().contains(root)) {
				std::fputs("PRIVATE_CONFIG_ERROR_DIALOG_SHOWN\n", stdout);
				std::fflush(stdout);
				dialog->accept();
			}
		});
	}
	Database database("private-child");
	return database.connection().databaseName() == expected ? 0 : 42;
}

class TestDatabasePath : public QObject {
	Q_OBJECT
private slots:
	void explicitFileAndDefault_data() {
		QTest::addColumn< QString >("mode");
		QTest::newRow("explicit-existing-file") << QString("file");
		QTest::newRow("unconfigured-default") << QString("default");
	}
	void explicitFileAndDefault() {
		QFETCH(QString, mode);
		QTemporaryDir root;
		QVERIFY(root.isValid());
		QVERIFY(seedDatabase(root.filePath("mumble.sqlite")));
		QVERIFY(seedDatabase(root.filePath("explicit.sqlite")));
		const QByteArray unselectedHash = fileHash(
			root.filePath(mode == "default" ? QStringLiteral("explicit.sqlite") : QStringLiteral("mumble.sqlite")));
		QVERIFY(!unselectedHash.isEmpty());
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(),
					{ "-platform", "offscreen", "--database-child", root.path(), mode });
		QVERIFY(child.waitForStarted());
		QVERIFY(child.waitForFinished(10000));
		QCOMPARE(child.exitStatus(), QProcess::NormalExit);
		QCOMPARE(child.exitCode(), 0);
		QCOMPARE(fileHash(root.filePath(mode == "default" ? QStringLiteral("explicit.sqlite")
														  : QStringLiteral("mumble.sqlite"))),
				 unselectedHash);
	}
	void configuredDirectoryCannotFallBack_data() {
		QTest::addColumn< QString >("mode");
		QTest::newRow("default-qt-handler") << QString("directory");
		QTest::newRow("production-app-handler") << QString("directory-app-logger");
	}
	void configuredDirectoryCannotFallBack() {
		QFETCH(QString, mode);
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const QString fallback = root.filePath("mumble.sqlite");
		QVERIFY(seedDatabase(fallback));
		const QByteArray before = fileHash(fallback);
		QVERIFY(!before.isEmpty());
		QProcess child;
		// Windows' Qt default handler otherwise routes a child without a console
		// to OutputDebugString. Capture the diagnostic without changing fatal exit.
		QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
		environment.insert("QT_FORCE_STDERR_LOGGING", "1");
		child.setProcessEnvironment(environment);
		child.start(QCoreApplication::applicationFilePath(),
					{ "-platform", "offscreen", "--database-child", root.path(), mode });
		QVERIFY(child.waitForStarted());
		QVERIFY(child.waitForFinished(10000));
		if (mode == "directory-app-logger") {
			QCOMPARE(child.exitStatus(), QProcess::NormalExit);
			QCOMPARE(child.exitCode(), 1);
		} else {
			QCOMPARE(child.exitStatus(), QProcess::CrashExit);
			QVERIFY(child.exitCode() != 0);
		}
		const QByteArray output = child.readAllStandardError() + child.readAllStandardOutput();
		QVERIFY(output.contains("Database: Unable to open configured database"));
		QVERIFY(output.contains("PRIVATE_CONFIG_ERROR_DIALOG_SHOWN"));
		QCOMPARE(fileHash(fallback), before);
	}
};

int main(int argc, char **argv) {
	QStandardPaths::setTestModeEnabled(true);
	QApplication app(argc, argv);
	const QStringList args = app.arguments();
	if (args.size() == 4 && args.at(1) == "--database-child")
		return databaseChild(args.at(2), args.at(3));
	TestDatabasePath test;
	return QTest::qExec(&test, argc, argv);
}
#include "TestDatabasePath.moc"
