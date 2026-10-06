// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareViewer.h"
#include "VideoViewUi.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QCloseEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

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
private:
	/// Paints a video frame directly into the available area without allocating a newly scaled
	/// pixmap for every incoming frame. Letterboxing keeps screen content and camera feeds
	/// uncropped. Nested inside ScreenShareTile so both share its linkage.
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
			painter.fillRect(rect(), palette().color(QPalette::Base));

			if (m_frame.isNull()) {
				painter.setPen(palette().color(QPalette::Text));
				painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
								 tr("Connecting to video…"));
				return;
			}

			QSize frameSize = m_frame.size();
			frameSize.scale(size(), Qt::KeepAspectRatio);
			const QRect target(QPoint((width() - frameSize.width()) / 2, (height() - frameSize.height()) / 2),
							   frameSize);

			painter.setRenderHint(QPainter::SmoothPixmapTransform);
			painter.drawImage(target, m_frame);
		}

	private:
		QImage m_frame;
	};

public:
	explicit ScreenShareTile(const QString &senderName, QWidget *parent = nullptr) : QFrame(parent) {
		setObjectName(QStringLiteral("videoTile"));
		setFrameStyle(QFrame::StyledPanel);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

		m_video = new VideoFrameWidget(this);
		m_name  = new QLabel(senderName, this);
		m_name->setTextFormat(Qt::PlainText);
		m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
		m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		m_name->setAlignment(Qt::AlignCenter);
		m_name->setToolTip(senderName);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(8);
		layout->addWidget(m_video, 1);
		layout->addWidget(m_name);
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

	VideoViewUi::style(this);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(16, 12, 16, 12);
	layout->setSpacing(12);
	auto *toolbar = new QHBoxLayout;
	m_status      = new QLabel(tr("Shared video"), this);
	m_status->setObjectName(QStringLiteral("videoHeading"));
	toolbar->addWidget(m_status);
	toolbar->addStretch();
	m_focusButton = new QPushButton(tr("Focus view"), this);
	m_focusButton->setCheckable(true);
	m_focusButton->setToolTip(tr("Enlarge one participant's video"));
	connect(m_focusButton, &QPushButton::toggled, this, &ScreenShareViewer::setFocused);
	toolbar->addWidget(m_focusButton);
	toolbar->addWidget(VideoViewUi::fullscreenButton(this));
	layout->addLayout(toolbar);

	m_participants = new QComboBox(this);
	m_participants->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_participants->setMinimumContentsLength(16);
	m_participants->setAccessibleName(tr("Video to focus"));
	m_participants->setToolTip(tr("Choose the participant to focus on"));
	m_participants->hide();
	connect(m_participants, QOverload< int >::of(&QComboBox::currentIndexChanged), this, [this](int) {
		m_columnCount = 0;
		reflowTiles();
	});
	auto *participantRow = new QHBoxLayout;
	participantRow->addWidget(m_participants);
	participantRow->addStretch();
	layout->addLayout(participantRow);

	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setFrameShape(QFrame::NoFrame);
	m_scrollArea->setWidgetResizable(true);
	auto *stage  = new QWidget(m_scrollArea);
	m_gridLayout = new QGridLayout(stage);
	m_gridLayout->setContentsMargins(0, 0, 0, 0);
	m_gridLayout->setSpacing(12);
	m_scrollArea->setWidget(stage);
	m_scrollArea->viewport()->installEventFilter(this);
	layout->addWidget(m_scrollArea, 1);
	setMinimumSize(480, 320);

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
		if (tile->senderName() != senderName) {
			tile->setSenderName(senderName);
			m_participants->setItemText(m_participants->findData(senderSession), senderName);
		}
		return tile;
	}

	tile = new ScreenShareTile(senderName, this);
	m_tiles.insert(senderSession, tile);
	{
		const QSignalBlocker blocker(m_participants);
		m_participants->addItem(senderName, senderSession);
	}
	m_columnCount = 0;
	reflowTiles();
	updateWindowTitle();
	return tile;
}

void ScreenShareViewer::showAndRefresh(quint32 senderSession, const QString &senderName) {
	ensureTile(senderSession, senderName);
	if (m_focused)
		m_participants->setCurrentIndex(m_participants->findData(senderSession));
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
	tile->hide();
	tile->deleteLater();
	{
		const QSignalBlocker blocker(m_participants);
		m_participants->removeItem(m_participants->findData(senderSession));
	}
	m_columnCount = 0;
	reflowTiles();
	updateWindowTitle();

	if (m_tiles.isEmpty())
		hide();
}

void ScreenShareViewer::clearStreams() {
	const QList< ScreenShareTile * > tiles = m_tiles.values();
	m_tiles.clear();
	{
		const QSignalBlocker blocker(m_participants);
		m_participants->clear();
	}
	for (ScreenShareTile *tile : tiles) {
		m_gridLayout->removeWidget(tile);
		tile->hide();
		tile->deleteLater();
	}
	m_columnCount = 0;
	updateWindowTitle();
	hide();
}

void ScreenShareViewer::setFocused(bool focused) {
	m_focused = focused;
	m_focusButton->setText(focused ? tr("Gallery view") : tr("Focus view"));
	m_participants->setVisible(focused);
	m_columnCount = 0;
	reflowTiles();
}

void ScreenShareViewer::updateWindowTitle() {
	m_status->setText(tr("Shared video · %1").arg(m_tiles.size()));
	m_focusButton->setEnabled(!m_tiles.isEmpty());
	if (m_tiles.size() > 1)
		setWindowTitle(tr("Shared video — %1 participants").arg(m_tiles.size()));
	else
		setWindowTitle(tr("Shared video"));
}

void ScreenShareViewer::reflowTiles() {
	const int tileCount = static_cast< int >(m_tiles.size());
	const int columns   = m_focused ? 1
									: std::min(columnCountFor(tileCount, m_scrollArea->viewport()->size()),
											   std::max(1, (m_scrollArea->viewport()->width() + 12) / 268));
	// Focus view contains one layout item even when multiple streams are known.
	const int visibleTileCount = m_focused ? std::min(tileCount, 1) : tileCount;
	if (columns == m_columnCount && m_gridLayout->count() == visibleTileCount)
		return;

	for (int row = 0; row < m_gridLayout->rowCount(); ++row)
		m_gridLayout->setRowStretch(row, 0);
	for (int column = 0; column < m_gridLayout->columnCount(); ++column)
		m_gridLayout->setColumnStretch(column, 0);
	while (QLayoutItem *item = m_gridLayout->takeAt(0))
		delete item;

	int index = 0;
	for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
		ScreenShareTile *tile = it.value();
		const bool visible    = !m_focused || it.key() == m_participants->currentData().toUInt();
		tile->setVisible(visible);
		if (!visible)
			continue;
		m_gridLayout->addWidget(tile, index / columns, index % columns);
		m_gridLayout->setRowStretch(index / columns, 1);
		m_gridLayout->setColumnStretch(index % columns, 1);
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

bool ScreenShareViewer::eventFilter(QObject *watched, QEvent *event) {
	if (watched == m_scrollArea->viewport() && event->type() == QEvent::Resize)
		reflowTiles();
	return QDialog::eventFilter(watched, event);
}
