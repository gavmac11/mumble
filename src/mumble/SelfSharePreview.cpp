// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the Mumble source
// tree or at <https://www.mumble.info/LICENSE>.

#include "SelfSharePreview.h"
#include "VideoViewUi.h"

#include <QtGui/QResizeEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

SelfSharePreview::SelfSharePreview(QWidget *parent) : QDialog(parent, Qt::Window | Qt::WindowStaysOnTopHint) {
	setAttribute(Qt::WA_DeleteOnClose, false);
	VideoViewUi::style(this);

	m_imageLabel = new QLabel(this);
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_imageLabel->setMinimumSize(240, 135);
	m_imageLabel->setText(tr("Starting your video…"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	layout->addWidget(m_imageLabel, 1);
	auto *controls = new QHBoxLayout;
	m_status       = new QLabel(this);
	m_status->setObjectName(QStringLiteral("sharingStatus"));
	controls->addWidget(m_status, 1);
	auto *stop = new QPushButton(tr("Stop sharing"), this);
	stop->setObjectName(QStringLiteral("stopSharing"));
	connect(stop, &QPushButton::clicked, this, &SelfSharePreview::stopSharingRequested);
	controls->addWidget(stop);
	layout->addLayout(controls);
	auto *hint = new QLabel(tr("Closing this preview keeps sharing on."), this);
	hint->setObjectName(QStringLiteral("videoHint"));
	hint->setWordWrap(true);
	layout->addWidget(hint);

	resize(480, 320);
}

void SelfSharePreview::startSharing(bool isWebcam) {
	setWindowTitle(isWebcam ? tr("Your camera") : tr("Your screen"));
	m_status->setText(isWebcam ? tr("● Camera is on") : tr("● You are sharing"));
	// Drop any frame from a previous share so reopening never flashes stale content.
	m_currentFrame = QImage();
	m_imageLabel->setText(tr("Starting your video…"));
	show();
	raise();
}

void SelfSharePreview::showAndRefresh() {
	show();
	raise();
	activateWindow();
	updateImageDisplay();
}

void SelfSharePreview::updateFrame(QImage frame) {
	if (frame.isNull())
		return;

	m_currentFrame = frame;

	// Always store the latest frame so the preview shows it when re-opened via the menu.
	if (isVisible())
		updateImageDisplay();
}

void SelfSharePreview::updateImageDisplay() {
	if (m_currentFrame.isNull())
		return;

	QSize areaSize = m_imageLabel->contentsRect().size();
	QPixmap scaled = QPixmap::fromImage(m_currentFrame).scaled(areaSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	m_imageLabel->setPixmap(scaled);
}

void SelfSharePreview::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);
	updateImageDisplay();
}
