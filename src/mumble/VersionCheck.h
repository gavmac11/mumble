// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VERSIONCHECK_H_
#define MUMBLE_MUMBLE_VERSIONCHECK_H_

#include <QByteArray>
#include <QObject>

class VersionCheck : public QObject {
	Q_OBJECT
	Q_DISABLE_COPY(VersionCheck)

private:
	bool m_autoCheck;
	QByteArray m_response;
	void performRequest();
	void showResult();

public:
	VersionCheck(bool autocheck, QObject *parent = nullptr);
};

#endif
