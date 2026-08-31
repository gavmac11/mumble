// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_
#define MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_

#include <QtCore/QMap>
#include <QtGui/QImage>
#include <QtWidgets/QDialog>

class QGridLayout;
class ScreenShareTile;

/// Floating gallery that displays shared video from remote users.
/// A single stream fills the window; additional streams are arranged in a responsive grid.
class ScreenShareViewer : public QDialog {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ScreenShareViewer)

public:
	explicit ScreenShareViewer(QWidget *parent = nullptr);

	/// Returns true once the user has explicitly closed the window.
	/// While dismissed, new frames update the tiles but do not reopen the window.
	bool isDismissed() const;
	/// Show the gallery, adding a waiting tile for the requested sender when necessary.
	void showAndRefresh(quint32 senderSession, const QString &senderName);
	/// Remove a sender's tile after their stream ends.
	void removeStream(quint32 senderSession);
	/// Remove all tiles, for example after disconnecting from a server.
	void clearStreams();

public slots:
	void updateFrame(quint32 senderSession, const QString &senderName, QImage frame);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	ScreenShareTile *ensureTile(quint32 senderSession, const QString &senderName);
	void reflowTiles();
	void updateWindowTitle();

	QGridLayout *m_gridLayout;
	QMap< quint32, ScreenShareTile * > m_tiles;
	bool m_dismissed  = false;
	int m_columnCount = 0;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_
