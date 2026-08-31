// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareViewer.h"

#include <QtGui/QCloseEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

/// Paints a video frame directly into the available area without allocating a newly scaled
/// pixmap for every incoming frame. Letterboxing keeps screen content and camera feeds uncropped.
class VideoFrameWidget : public QWidget {
public:
	explicit VideoFrameWidget(QWidget *parent = nullptr) : QWidget(parent) {
		setMinimumSize(240, 135);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	void setFrame(const QImage &frame) {
		m_frame = frame;
		update();
	}

	const QImage &frame() const { return m_frame; }

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.fillRect(rect(), palette().color(QPalette::Dark));

		if (m_frame.isNull()) {
			painter.setPen(palette().color(QPalette::BrightText));
			painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
							 tr("Waiting for first frame…"));
			return;
		}

		QSize frameSize = m_frame.size();
		frameSize.scale(size(), Qt::KeepAspectRatio);
		const QRect target(QPoint((width() - frameSize.width()) / 2, (height() - frameSize.height()) / 2), frameSize);

		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.drawImage(target, m_frame);
	}

private:
	QImage m_frame;
};

int columnCountFor(int tileCount, const QSize &availableSize) {
	if (tileCount <= 1)
		return 1;

	// Keep pairs side-by-side in a typical landscape window. In a narrow or portrait window,
	// one column gives each feed enough width to remain legible.
	if (tileCount == 2)
		return availableSize.width() >= availableSize.height() ? 2 : 1;

	const int squareGridColumns = static_cast< int >(std::ceil(std::sqrt(static_cast< double >(tileCount))));
	return availableSize.width() >= availableSize.height() ? squareGridColumns : std::max(2, squareGridColumns - 1);
}

} // namespace

class ScreenShareTile : public QFrame {
public:
	explicit ScreenShareTile(const QString &senderName, QWidget *parent = nullptr) : QFrame(parent) {
		setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

		m_video = new VideoFrameWidget(this);
		m_name  = new QLabel(senderName, this);
		m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
		m_name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		m_name->setToolTip(senderName);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 8);
		layout->setSpacing(8);
		layout->addWidget(m_video, 1);
		layout->addWidget(m_name);
		layout->setAlignment(m_name, Qt::AlignHCenter);
	}

	void setSenderName(const QString &senderName) {
		m_name->setText(senderName);
		m_name->setToolTip(senderName);
	}

	void setFrame(const QImage &frame) { m_video->setFrame(frame); }

	QString senderName() const { return m_name->text(); }
	QImage frame() const { return m_video->frame(); }

	void refresh() { m_video->update(); }

private:
	VideoFrameWidget *m_video;
	QLabel *m_name;
};

ScreenShareViewer::ScreenShareViewer(QWidget *parent) : QDialog(parent, Qt::Window) {
	setWindowTitle(tr("Shared video"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setMinimumSize(480, 320);

	m_gridLayout = new QGridLayout(this);
	m_gridLayout->setContentsMargins(8, 8, 8, 8);
	m_gridLayout->setHorizontalSpacing(8);
	m_gridLayout->setVerticalSpacing(8);

	resize(960, 640);
}

bool ScreenShareViewer::isDismissed() const {
	return m_dismissed;
}

void ScreenShareViewer::addStream(quint32 senderSession, const QString &senderName, const QImage &frame) {
	ScreenShareTile *tile = ensureTile(senderSession, senderName);
	if (!frame.isNull())
		tile->setFrame(frame);
}

QList< ScreenShareViewer::StreamInfo > ScreenShareViewer::streams() const {
	QList< StreamInfo > result;
	for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
		result.append({ it.key(), it.value()->senderName(), it.value()->frame() });
	}
	return result;
}

ScreenShareTile *ScreenShareViewer::ensureTile(quint32 senderSession, const QString &senderName) {
	ScreenShareTile *tile = m_tiles.value(senderSession, nullptr);
	if (tile) {
		tile->setSenderName(senderName);
		return tile;
	}

	tile = new ScreenShareTile(senderName, this);
	m_tiles.insert(senderSession, tile);
	reflowTiles();
	updateWindowTitle();
	return tile;
}

void ScreenShareViewer::showAndRefresh(quint32 senderSession, const QString &senderName) {
	ensureTile(senderSession, senderName);
	m_dismissed = false;
	show();
	raise();
	activateWindow();

	for (ScreenShareTile *tile : m_tiles)
		tile->refresh();
}

void ScreenShareViewer::updateFrame(quint32 senderSession, const QString &senderName, QImage frame) {
	if (frame.isNull())
		return;

	ensureTile(senderSession, senderName)->setFrame(frame);
}

void ScreenShareViewer::removeStream(quint32 senderSession) {
	ScreenShareTile *tile = m_tiles.take(senderSession);
	if (!tile)
		return;

	m_gridLayout->removeWidget(tile);
	tile->deleteLater();
	m_columnCount = 0;
	reflowTiles();
	updateWindowTitle();

	if (m_tiles.isEmpty())
		hide();
}

void ScreenShareViewer::clearStreams() {
	const QList< ScreenShareTile * > tiles = m_tiles.values();
	m_tiles.clear();
	for (ScreenShareTile *tile : tiles) {
		m_gridLayout->removeWidget(tile);
		tile->deleteLater();
	}
	m_columnCount = 0;
	updateWindowTitle();
	hide();
}

void ScreenShareViewer::updateWindowTitle() {
	if (m_tiles.size() > 1)
		setWindowTitle(tr("Shared video — %1 participants").arg(m_tiles.size()));
	else
		setWindowTitle(tr("Shared video"));
}

void ScreenShareViewer::reflowTiles() {
	const int tileCount = static_cast< int >(m_tiles.size());
	const int columns   = columnCountFor(tileCount, size());
	if (columns == m_columnCount && m_gridLayout->count() == tileCount)
		return;

	while (QLayoutItem *item = m_gridLayout->takeAt(0))
		delete item;

	int index = 0;
	for (ScreenShareTile *tile : m_tiles) {
		m_gridLayout->addWidget(tile, index / columns, index % columns);
		++index;
	}

	m_columnCount = columns;
}

void ScreenShareViewer::closeEvent(QCloseEvent *event) {
	// Remember that the user explicitly closed the gallery so new frames don't reopen it.
	m_dismissed = true;
	QDialog::closeEvent(event);
}

void ScreenShareViewer::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);
	reflowTiles();
}
