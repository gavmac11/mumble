// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ReleaseUpdate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ReleaseUpdate {

QString repository() {
	return QStringLiteral("gavmac11/mumble");
}

QString assetForPlatform(const QString &os, const QString &architecture, const QString &osVersion) {
	if (os == QLatin1String("windows")
		&& (architecture == QLatin1String("x86_64") || architecture == QLatin1String("x64"))) {
		return QStringLiteral("Mumble-Windows-x64.exe");
	}
	if (os == QLatin1String("macos") && architecture == QLatin1String("arm64")
		&& QVersionNumber::fromString(osVersion) >= QVersionNumber(15)) {
		return QStringLiteral("Mumble-macOS-arm64.zip");
	}
	if (os == QLatin1String("ubuntu")
		&& (architecture == QLatin1String("x86_64") || architecture == QLatin1String("x64"))
		&& osVersion == QLatin1String("24.04")) {
		return QStringLiteral("Mumble-Ubuntu-24.04-amd64.deb");
	}
	return {};
}

Result parse(const QByteArray &json, const QString &assetName, bool includePreviews) {
	const QJsonDocument document = QJsonDocument::fromJson(json);
	Result result;
	QJsonArray releases;
	if (document.isArray()) {
		releases = document.array();
	} else if (!includePreviews && document.isObject() && document.object().contains(QLatin1String("tag_name"))) {
		// The stable channel uses /releases/latest, which returns one object.
		releases.append(document.object());
	} else {
		return result;
	}
	result.valid = true;
	const QRegularExpression tagPattern(QStringLiteral("^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
	for (const QJsonValue &value : releases) {
		const QJsonObject object = value.toObject();
		if (!object.value(QLatin1String("draft")).isBool() || object.value(QLatin1String("draft")).toBool()
			|| !object.value(QLatin1String("prerelease")).isBool()) {
			continue;
		}
		const bool preview = object.value(QLatin1String("prerelease")).toBool();
		const QString tag  = object.value(QLatin1String("tag_name")).toString();
		if ((preview && !includePreviews) || !tagPattern.match(tag).hasMatch()) {
			continue;
		}
		const QVersionNumber version = QVersionNumber::fromString(tag.mid(1));
		if (version.segmentCount() != 3 || version <= result.release.version) {
			continue;
		}
		const QString base = QStringLiteral("https://github.com/%1/releases/").arg(repository());
		const QUrl notes(base + QLatin1String("tag/") + tag);
		if (object.value(QLatin1String("html_url")).toString() != notes.toString()) {
			continue;
		}
		const QString downloadBase = base + QLatin1String("download/") + tag + QLatin1Char('/');
		QUrl download;
		bool hasChecksums = false;
		for (const QJsonValue &assetValue : object.value(QLatin1String("assets")).toArray()) {
			const QJsonObject asset = assetValue.toObject();
			const QString name      = asset.value(QLatin1String("name")).toString();
			if (asset.value(QLatin1String("state")).toString() != QLatin1String("uploaded")
				|| asset.value(QLatin1String("size")).toDouble() <= 0
				|| asset.value(QLatin1String("browser_download_url")).toString() != downloadBase + name) {
				continue;
			}
			if (name == QLatin1String("SHA256SUMS.txt")) {
				hasChecksums = true;
			}
			if (!assetName.isEmpty() && name == assetName) {
				download = QUrl(downloadBase + name);
			}
		}
		// Ignore incomplete releases for platforms on which we ship a package.
		if (hasChecksums && (assetName.isEmpty() || !download.isEmpty())) {
			result.release = { version, notes, download, preview };
		}
	}
	return result;
}

} // namespace ReleaseUpdate
