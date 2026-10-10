// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "Database.h"
#include "Global.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

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
	global.s.qsDatabaseLocation = mode == "default" ? QString() : (mode == "directory" ? root : expected);
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
	void configuredDirectoryCannotFallBack() {
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const QString fallback = root.filePath("mumble.sqlite");
		QVERIFY(seedDatabase(fallback));
		const QByteArray before = fileHash(fallback);
		QVERIFY(!before.isEmpty());
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(),
					{ "-platform", "offscreen", "--database-child", root.path(), "directory" });
		QVERIFY(child.waitForStarted());
		QVERIFY(child.waitForFinished(10000));
		QCOMPARE(child.exitStatus(), QProcess::CrashExit);
		QVERIFY(child.exitCode() != 0);
		QVERIFY(child.readAllStandardError().contains("Database: Unable to open configured database"));
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
