// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_RELEASEUPDATECACHE_H_
#define MUMBLE_MUMBLE_RELEASEUPDATECACHE_H_

#include <QByteArray>
#include <QString>

namespace ReleaseUpdate {

constexpr qsizetype MaximumResponseSize = 4 * 1024 * 1024;

// Persist the feed and request deadlines together, bound to one repository/channel URL.
class Cache {
public:
	Cache(const QString &path, const QString &endpoint, bool includePreviews);
	bool canRequest(bool automatic, qint64 now) const;
	void recordAttempt(qint64 now);
	bool acceptResponse(int status, const QByteArray &body, const QByteArray &etag);
	void rateLimited(qint64 now, const QByteArray &retryAfter, const QByteArray &reset);
	const QByteArray &body() const { return m_body; }
	const QByteArray &etag() const { return m_etag; }

private:
	void save() const;
	QString m_path;
	QString m_endpoint;
	bool m_includePreviews;
	QByteArray m_body;
	QByteArray m_etag;
	qint64 m_nextAutomaticCheck = 0;
	qint64 m_retryAfter         = 0;
};

} // namespace ReleaseUpdate

#endif
