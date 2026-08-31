// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREWINDOW_H_
#define MUMBLE_MUMBLE_SCREENSHAREWINDOW_H_

#include <QtGui/QImage>
#include <QtWidgets/QDialog>

class QLabel;

/// Floating window that displays the shared video stream from a single remote user.
/// Used when Settings::videoDisplayMode is SeparateWindows; the gallery
/// (ScreenShareViewer) is used otherwise.
class ScreenShareWindow : public QDialog {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ScreenShareWindow)

public:
	explicit ScreenShareWindow(quint32 senderSession, const QString &senderName, QWidget *parent = nullptr);

	/// Show the window and repaint with the last stored frame.
	void showAndRefresh();

	/// Name of the user this window belongs to.
	QString senderName() const;
	/// Latest received frame; null before the first frame arrives.
	QImage currentFrame() const;

public slots:
	void updateFrame(QImage frame);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;

private:
	void updateImageDisplay();

	QLabel *m_imageLabel;
	quint32 m_senderSession;
	QString m_senderName;
	QImage m_currentFrame;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREWINDOW_H_
