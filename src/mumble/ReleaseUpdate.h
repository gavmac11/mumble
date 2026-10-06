// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_RELEASEUPDATE_H_
#define MUMBLE_MUMBLE_RELEASEUPDATE_H_

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

namespace ReleaseUpdate {

struct Release {
	QVersionNumber version;
	QUrl notes;
	QUrl download;
	bool preview = false;
};

struct Result {
	bool valid = false;
	Release release;
};

QString repository();
QString assetForPlatform(const QString &os, const QString &architecture, const QString &osVersion);
// Select by numeric version, independent of publication order. An empty asset name means notes only.
Result parse(const QByteArray &json, const QString &assetName, bool includePreviews);

} // namespace ReleaseUpdate

#endif
