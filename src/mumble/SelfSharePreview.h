// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the Mumble source
// tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SELFSHAREPREVIEW_H_
#define MUMBLE_MUMBLE_SELFSHAREPREVIEW_H_

#include <QtGui/QImage>
#include <QtWidgets/QDialog>

class QLabel;
class QResizeEvent;

/// Small always-on-top window showing what the local user is currently sharing (webcam or
/// screen), fed from ScreenCapture's previewFrame() signal before encoding. Its whole point
/// is checking framing while the camera is being physically adjusted, so it deliberately
/// floats above everything else.
class SelfSharePreview : public QDialog {
private:
	Q_OBJECT
	Q_DISABLE_COPY(SelfSharePreview)

public:
	explicit SelfSharePreview(QWidget *parent = nullptr);

	/// Resets the frame and title for a newly started share and shows the window. Does not
	/// activate it, so starting a share does not steal focus.
	void startSharing(bool isWebcam);
	/// Reopens the window from a menu action: shows, raises and repaints the last frame.
	void showAndRefresh();

public slots:
	void updateFrame(QImage frame);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void updateImageDisplay();

	QLabel *m_imageLabel  = nullptr;
	QImage m_currentFrame;
};

#endif // MUMBLE_MUMBLE_SELFSHAREPREVIEW_H_
