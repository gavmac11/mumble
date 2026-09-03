// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareWindow.h"

#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

ScreenShareWindow::ScreenShareWindow(quint32 senderSession, const QString &senderName, QWidget *parent)
	: QDialog(parent, Qt::Window), m_senderSession(senderSession), m_senderName(senderName) {
	setWindowTitle(tr("%1's screen").arg(senderName));

	m_imageLabel = new QLabel(this);
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_imageLabel->setMinimumSize(320, 240);
	m_imageLabel->setText(tr("Waiting for first frame…"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_imageLabel);

	resize(800, 600);
}

void ScreenShareWindow::showAndRefresh() {
	show();
	raise();
	activateWindow();
	updateImageDisplay();
}

QString ScreenShareWindow::senderName() const {
	return m_senderName;
}

QImage ScreenShareWindow::currentFrame() const {
	return m_currentFrame;
}

void ScreenShareWindow::updateImageDisplay() {
	if (m_currentFrame.isNull())
		return;

	QSize areaSize = size();
	QPixmap scaled = QPixmap::fromImage(m_currentFrame).scaled(areaSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	m_imageLabel->setPixmap(scaled);
	m_imageLabel->resize(scaled.size());
}

void ScreenShareWindow::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);
	updateImageDisplay();
}

void ScreenShareWindow::showEvent(QShowEvent *event) {
	QDialog::showEvent(event);
	// updateFrame() only repaints while visible, so a window that received frames while
	// hidden (e.g. created by a display-mode switch) must paint its stored frame on show.
	updateImageDisplay();
}

void ScreenShareWindow::updateFrame(QImage frame) {
	if (frame.isNull())
		return;

	m_currentFrame = frame;

	// Always update the image data so the window shows the latest frame
	// when the user re-opens it via the context menu.
	if (isVisible())
		updateImageDisplay();
}
