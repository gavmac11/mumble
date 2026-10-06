// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VersionCheck.h"

#include "MainWindow.h"
#include "NetworkConfig.h"
#include "ReleaseUpdate.h"
#include "Version.h"
#include "Global.h"

#include <QDesktopServices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QtWidgets/QMessageBox>

VersionCheck::VersionCheck(bool autocheck, QObject *parent) : QObject(parent), m_autoCheck(autocheck) {
	// Queue the request so construction has finished before any completion path runs.
	QTimer::singleShot(0, this, &VersionCheck::performRequest);
}

void VersionCheck::performRequest() {
	const QString endpoint =
		MUMBLE_UPDATE_PRERELEASES ? QStringLiteral("releases?per_page=100") : QStringLiteral("releases/latest");
	QNetworkRequest request(
		QUrl(QStringLiteral("https://api.github.com/repos/%1/%2").arg(ReleaseUpdate::repository(), endpoint)));
	Network::prepareRequest(request);
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
	request.setTransferTimeout(15000);
	QNetworkReply *reply = Global::get().nam->get(request);
	// Bound both the total duration and response size, including a slowly trickling response.
	QTimer::singleShot(20000, reply, &QNetworkReply::abort);
	connect(this, &QObject::destroyed, reply, &QNetworkReply::abort);
	connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
		constexpr qsizetype maximumSize = 4 * 1024 * 1024;
		m_response += reply->read(maximumSize + 1 - m_response.size());
		if (m_response.size() > maximumSize) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		const bool success = reply->error() == QNetworkReply::NoError
							 && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
		reply->deleteLater();
		if (!success) {
			if (!m_autoCheck) {
				QMessageBox::warning(Global::get().mw, tr("Check for updates"),
									 tr("Could not check GitHub for updates. Please try again later."));
			}
			deleteLater();
			return;
		}
		showResult();
	});
}

void VersionCheck::showResult() {
	QString os = QString::fromLatin1(MUMBLE_TARGET_OS);
	if (os == QLatin1String("linux")) {
		os = QSysInfo::productType();
	}
	const QString asset =
		ReleaseUpdate::assetForPlatform(os, QString::fromLatin1(MUMBLE_TARGET_ARCH), QSysInfo::productVersion());
	const auto result = ReleaseUpdate::parse(m_response, asset, MUMBLE_UPDATE_PRERELEASES);
	const QVersionNumber current(MUMBLE_VERSION_MAJOR, MUMBLE_VERSION_MINOR, MUMBLE_VERSION_PATCH);
	if (!result.valid || result.release.version.isNull()) {
		if (!m_autoCheck) {
			QMessageBox::information(
				Global::get().mw, tr("Check for updates"),
				result.valid ? tr("No compatible versioned releases have been published for this update channel yet.")
							 : tr("GitHub returned an invalid update response. Please try again later."));
		}
	} else if (result.release.version <= current) {
		if (!m_autoCheck) {
			QMessageBox::information(Global::get().mw, tr("Check for updates"),
									 tr("Mumble %1 is up to date for this update channel.").arg(current.toString()));
		}
	} else {
		// Avoid duplicate startup notifications if a check is requested more than once in a session.
		static QVersionNumber lastNotified;
		if (!m_autoCheck || result.release.version > lastNotified) {
			lastNotified = result.release.version;
			QMessageBox box(Global::get().mw);
			box.setWindowTitle(tr("Mumble update available"));
			box.setIcon(QMessageBox::Information);
			box.setTextFormat(Qt::PlainText);
			box.setText(tr("Mumble %1%2 is available. You are using %3.")
							.arg(result.release.version.toString(),
								 result.release.preview ? tr(" (preview)") : QString(), current.toString()));
			box.setInformativeText(result.release.download.isEmpty()
									   ? tr("View the release notes for installation options on your platform.")
									   : tr("Download the new version, then close Mumble and install it. Your settings "
											"will be preserved."));
			auto *notes           = box.addButton(tr("Release notes"), QMessageBox::ActionRole);
			QPushButton *download = nullptr;
			if (!result.release.download.isEmpty()) {
				download = box.addButton(tr("Download update"), QMessageBox::AcceptRole);
			}
			box.addButton(tr("Later"), QMessageBox::RejectRole);
			box.exec();
			if (box.clickedButton() == notes) {
				QDesktopServices::openUrl(result.release.notes);
			} else if (download && box.clickedButton() == download) {
				QDesktopServices::openUrl(result.release.download);
			}
		}
	}
	deleteLater();
}
