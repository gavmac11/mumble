// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the Mumble source
// tree or at <https://www.mumble.info/LICENSE>.

#include "SelfSharePreview.h"

#include <QtGui/QResizeEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

SelfSharePreview::SelfSharePreview(QWidget *parent)
	: QDialog(parent, Qt::Window | Qt::WindowStaysOnTopHint) {
	setAttribute(Qt::WA_DeleteOnClose, false);

	m_imageLabel = new QLabel(this);
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_imageLabel->setMinimumSize(240, 160);
	m_imageLabel->setText(tr("Waiting for first frame…"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_imageLabel);

	resize(480, 320);
}

void SelfSharePreview::startSharing(bool isWebcam) {
	setWindowTitle(isWebcam ? tr("Your webcam (live)") : tr("Your screen (live)"));
	// Drop any frame from a previous share so reopening never flashes stale content.
	m_currentFrame = QImage();
	m_imageLabel->setText(tr("Waiting for first frame…"));
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

	QSize areaSize     = size();
	QPixmap scaled     = QPixmap::fromImage(m_currentFrame)
					.scaled(areaSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	m_imageLabel->setPixmap(scaled);
	m_imageLabel->resize(scaled.size());
}

void SelfSharePreview::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);
	updateImageDisplay();
}
