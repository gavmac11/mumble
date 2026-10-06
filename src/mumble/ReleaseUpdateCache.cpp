// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ReleaseUpdateCache.h"
#include "ReleaseUpdate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace ReleaseUpdate {
namespace {
	constexpr qint64 automaticInterval = 6 * 60 * 60;
	constexpr qint64 maximumBackoff    = 24 * 60 * 60;

	QByteArray validEtag(const QByteArray &etag) {
		return etag.size() <= 256 && !etag.contains('\r') && !etag.contains('\n') ? etag : QByteArray();
	}
} // namespace

Cache::Cache(const QString &path, const QString &endpoint, bool includePreviews)
	: m_path(path), m_endpoint(endpoint), m_includePreviews(includePreviews) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * MaximumResponseSize) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.read(2 * MaximumResponseSize + 1));
	const auto object   = document.object();
	if (object.value(QLatin1String("endpoint")).toString() != endpoint) {
		return;
	}
	const auto body = QByteArray::fromBase64(object.value(QLatin1String("body")).toString().toLatin1());
	if (body.size() > MaximumResponseSize || (!body.isEmpty() && !parse(body, {}, includePreviews).valid)) {
		return;
	}
	m_body = body;
	if (!body.isEmpty()) {
		m_etag = validEtag(object.value(QLatin1String("etag")).toString().toLatin1());
	}
	m_nextAutomaticCheck = object.value(QLatin1String("nextAutomaticCheck")).toString().toLongLong();
	m_retryAfter         = object.value(QLatin1String("retryAfter")).toString().toLongLong();
}

bool Cache::canRequest(bool automatic, qint64 now) const {
	// Discard implausible deadlines, including those caused by a clock moving backwards.
	const auto pending = [now](qint64 deadline) { return deadline > now && deadline <= now + maximumBackoff; };
	return !pending(m_retryAfter) && (!automatic || !pending(m_nextAutomaticCheck));
}

void Cache::recordAttempt(qint64 now) {
	// Persist before issuing the request, so failures and repeated launches are throttled too.
	m_nextAutomaticCheck = now + automaticInterval;
	save();
}

bool Cache::acceptResponse(int status, const QByteArray &body, const QByteArray &etag) {
	if (status == 304) {
		return !m_body.isEmpty();
	}
	if (status != 200 || body.size() > MaximumResponseSize || !parse(body, {}, m_includePreviews).valid) {
		return false;
	}
	m_body       = body;
	m_etag       = validEtag(etag);
	m_retryAfter = 0;
	save();
	return true;
}

void Cache::rateLimited(qint64 now, const QByteArray &retryAfter, const QByteArray &reset) {
	const qint64 delay     = retryAfter.toLongLong();
	const qint64 resetTime = reset.toLongLong();
	qint64 wait            = 60 * 60;
	if (delay > 0) {
		wait = std::min(delay, maximumBackoff);
	} else if (resetTime > now) {
		wait = std::min(resetTime - now, maximumBackoff);
	}
	m_retryAfter = now + wait;
	save();
}

void Cache::save() const {
	if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
		return;
	}
	const QJsonObject object{ { "endpoint", m_endpoint },
							  { "body", QString::fromLatin1(m_body.toBase64()) },
							  { "etag", QString::fromLatin1(m_etag) },
							  { "nextAutomaticCheck", QString::number(m_nextAutomaticCheck) },
							  { "retryAfter", QString::number(m_retryAfter) } };
	QSaveFile file(m_path);
	if (file.open(QIODevice::WriteOnly)) {
		const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
		if (file.write(data) == data.size()) {
			file.commit();
		}
	}
}

} // namespace ReleaseUpdate
