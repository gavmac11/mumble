// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ReleaseUpdate.h"
#include "ReleaseUpdateCache.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {
const QString package = QStringLiteral("Mumble-Windows-x64.exe");
QJsonObject release(const QString &version, bool preview = true) {
	const QString base = QStringLiteral("https://github.com/gavmac11/mumble/releases/");
	const QString tag  = QLatin1Char('v') + version;
	QJsonArray assets;
	for (const QString &name : { package, QStringLiteral("SHA256SUMS.txt") }) {
		assets.append(QJsonObject{
			{ "name", name },
			{ "state", "uploaded" },
			{ "size", 42 },
			{ "browser_download_url", QString(base + QStringLiteral("download/") + tag + QLatin1Char('/') + name) } });
	}
	return { { "tag_name", tag },
			 { "draft", false },
			 { "prerelease", preview },
			 { "html_url", QString(base + QStringLiteral("tag/") + tag) },
			 { "assets", assets } };
}
ReleaseUpdate::Result parse(const QJsonArray &releases, bool previews = true) {
	return ReleaseUpdate::parse(QJsonDocument(releases).toJson(), package, previews);
}
} // namespace

class TestReleaseUpdate : public QObject {
	Q_OBJECT
private slots:
	void ordersVersionsNumerically() {
		const auto result = parse({ release("1.7.9"), release("1.7.100"), release("1.7.11") });
		QVERIFY(result.valid);
		QCOMPARE(result.release.version, QVersionNumber(1, 7, 100));
		QVERIFY(result.release.version > QVersionNumber(1, 7, 99));
		QVERIFY(!(result.release.version > QVersionNumber(1, 7, 100)));
		QVERIFY(!(result.release.version > QVersionNumber(1, 8, 0)));
	}
	void separatesChannels() {
		const QJsonArray releases{ release("1.7.100", false), release("1.7.101") };
		QCOMPARE(parse(releases).release.version, QVersionNumber(1, 7, 101));
		QCOMPARE(parse(releases, false).release.version, QVersionNumber(1, 7, 100));
		QVERIFY(parse({ release("1.7.101") }, false).release.version.isNull());
		const auto stable = ReleaseUpdate::parse(QJsonDocument(release("1.7.100", false)).toJson(), package, false);
		QCOMPARE(stable.release.version, QVersionNumber(1, 7, 100));
	}
	void ignoresDraftsLegacyTagsAndIncompletePackages() {
		auto draft            = release("1.7.105");
		draft["draft"]        = true;
		auto legacy           = release("1.7.104");
		legacy["tag_name"]    = "master-preview";
		auto incomplete       = release("1.7.103");
		incomplete["assets"]  = QJsonArray();
		auto noPackage        = release("1.7.102");
		noPackage["assets"]   = QJsonArray{ noPackage["assets"].toArray().at(1) };
		auto noChecksums      = release("1.7.101");
		noChecksums["assets"] = QJsonArray{ noChecksums["assets"].toArray().at(0) };
		QCOMPARE(parse({ draft, legacy, incomplete, noPackage, noChecksums, release("1.7.100") }).release.version,
				 QVersionNumber(1, 7, 100));
	}
	void rejectsUntrustedLinks() {
		auto badNotes        = release("1.7.101");
		badNotes["html_url"] = "https://github.com/another/repo/releases/tag/v1.7.101";
		QVERIFY(parse({ badNotes }).release.version.isNull());
		for (const QString &url :
			 { QStringLiteral("https://evil.example/Mumble-Windows-x64.exe"),
			   QStringLiteral("https://github.com/another/repo/releases/download/v1.7.101/Mumble-Windows-x64.exe"),
			   QStringLiteral("http://github.com/gavmac11/mumble/releases/download/v1.7.101/Mumble-Windows-x64.exe"),
			   QStringLiteral(
				   "https://github.com/gavmac11/mumble/releases/download/v1.7.100/Mumble-Windows-x64.exe") }) {
			auto badDownload              = release("1.7.101");
			auto assets                   = badDownload["assets"].toArray();
			auto asset                    = assets[0].toObject();
			asset["browser_download_url"] = url;
			assets[0]                     = asset;
			badDownload["assets"]         = assets;
			QVERIFY(parse({ badDownload }).release.version.isNull());
		}
	}
	void handlesEmptyAndMalformedFeeds() {
		QVERIFY(!ReleaseUpdate::parse("broken json", package, true).valid);
		QVERIFY(!ReleaseUpdate::parse("{\"message\":\"rate limit\"}", package, true).valid);
		QVERIFY(!ReleaseUpdate::parse("{\"message\":\"rate limit\"}", package, false).valid);
		QVERIFY(parse({}).valid);
		QVERIFY(parse({}).release.version.isNull());
		for (const QString &version : { QStringLiteral("1.7"), QStringLiteral("1.7.1-extra"), QStringLiteral("01.7.1"),
										QStringLiteral("1.7.999999999999999999") }) {
			QVERIFY(parse({ release(version) }).release.version.isNull());
		}
	}
	void choosesOnlySupportedPlatformPackages() {
		using ReleaseUpdate::assetForPlatform;
		QCOMPARE(assetForPlatform("windows", "x86_64", "11"), package);
		QCOMPARE(assetForPlatform("windows", "x64", "11"), package);
		QCOMPARE(assetForPlatform("ubuntu", "x64", "24.04"), QStringLiteral("Mumble-Ubuntu-24.04-amd64.deb"));
		QVERIFY(assetForPlatform("macos", "arm64", "14.7").isEmpty());
		QVERIFY(assetForPlatform("macos", "arm64", "").isEmpty());
		QCOMPARE(assetForPlatform("macos", "arm64", "26.0"), QStringLiteral("Mumble-macOS-arm64.zip"));
		QCOMPARE(assetForPlatform("macos", "arm64", "15.0"), QStringLiteral("Mumble-macOS-arm64.zip"));
		QCOMPARE(assetForPlatform("ubuntu", "x86_64", "24.04"), QStringLiteral("Mumble-Ubuntu-24.04-amd64.deb"));
		QVERIFY(assetForPlatform("ubuntu", "arm64", "24.04").isEmpty());
		QVERIFY(assetForPlatform("ubuntu", "x86_64", "22.04").isEmpty());
		QVERIFY(assetForPlatform("debian", "x86_64", "12").isEmpty());
		QVERIFY(assetForPlatform("macos", "x86_64", "15.0").isEmpty());
		const auto unsupported =
			ReleaseUpdate::parse(QJsonDocument(QJsonArray{ release("1.7.100") }).toJson(), {}, true);
		QVERIFY(unsupported.release.download.isEmpty());
		QVERIFY(!unsupported.release.notes.isEmpty());
	}
	void persistsConditionalResponsesAndSeparatesFeeds() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path    = dir.filePath("feed.json");
		const QByteArray body = QJsonDocument(QJsonArray{ release("1.7.100") }).toJson();
		ReleaseUpdate::Cache cache(path, "preview", true);
		QVERIFY(!cache.acceptResponse(304, {}, {}));
		QVERIFY(cache.acceptResponse(200, body, "\"etag-1\""));
		ReleaseUpdate::Cache reloaded(path, "preview", true);
		QCOMPARE(reloaded.etag(), QByteArray("\"etag-1\""));
		QVERIFY(reloaded.acceptResponse(304, {}, {}));
		QCOMPARE(reloaded.body(), body);
		ReleaseUpdate::Cache otherFeed(path, "stable", false);
		QVERIFY(otherFeed.etag().isEmpty());
		QVERIFY(otherFeed.body().isEmpty());
		QVERIFY(!otherFeed.acceptResponse(304, {}, {}));
	}
	void throttlesFailedAttemptsAcrossLaunchesButAllowsManualChecks() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path   = dir.filePath("feed.json");
		constexpr qint64 now = 1000000;
		ReleaseUpdate::Cache cache(path, "preview", true);
		QVERIFY(cache.canRequest(true, now));
		cache.recordAttempt(now);
		ReleaseUpdate::Cache reloaded(path, "preview", true);
		QVERIFY(!reloaded.canRequest(true, now + 1));
		QVERIFY(reloaded.canRequest(false, now + 1));
		QVERIFY(reloaded.canRequest(true, now + 6 * 60 * 60));
		// A backwards clock jump must not disable checks indefinitely.
		QVERIFY(reloaded.canRequest(true, now - 2 * 24 * 60 * 60));
	}
	void preservesGoodCacheOnInvalidResponses() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path = dir.filePath("feed.json");
		ReleaseUpdate::Cache cache(path, "preview", true);
		QVERIFY(cache.acceptResponse(200, "[]", "good"));
		QVERIFY(!cache.acceptResponse(200, "not json", "bad"));
		QVERIFY(!cache.acceptResponse(403, "{}", "bad"));
		QVERIFY(!cache.acceptResponse(200, QByteArray(ReleaseUpdate::MaximumResponseSize + 1, ' '), "bad"));
		ReleaseUpdate::Cache reloaded(path, "preview", true);
		QCOMPARE(reloaded.etag(), QByteArray("good"));
		QCOMPARE(reloaded.body(), QByteArray("[]"));
		QVERIFY(reloaded.acceptResponse(200, "[]", "bad\r\nheader"));
		QVERIFY(reloaded.etag().isEmpty());
	}
	void recoversFromCorruptCache() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path = dir.filePath("feed.json");
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("incomplete cache file");
		file.close();
		ReleaseUpdate::Cache cache(path, "preview", true);
		QVERIFY(cache.canRequest(true, 1000000));
		QVERIFY(cache.etag().isEmpty());
		QVERIFY(cache.acceptResponse(200, "[]", "recovered"));
	}
	void honorsPersistedServerBackoffForManualChecks() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path   = dir.filePath("feed.json");
		constexpr qint64 now = 1000000;
		ReleaseUpdate::Cache cache(path, "preview", true);
		cache.rateLimited(now, "120", "");
		ReleaseUpdate::Cache reloaded(path, "preview", true);
		QVERIFY(!reloaded.canRequest(false, now + 119));
		QVERIFY(reloaded.canRequest(false, now + 120));
		reloaded.rateLimited(now, "", QByteArray::number(now + 180));
		QVERIFY(!reloaded.canRequest(false, now + 179));
		QVERIFY(reloaded.canRequest(false, now + 180));
		reloaded.rateLimited(now, "bad", "bad");
		QVERIFY(!reloaded.canRequest(false, now + 3599));
		QVERIFY(reloaded.canRequest(false, now + 3600));
		reloaded.rateLimited(now, "99999999999", "");
		QVERIFY(reloaded.canRequest(false, now + 24 * 60 * 60));
	}
};

QTEST_GUILESS_MAIN(TestReleaseUpdate)
#include "TestReleaseUpdate.moc"
