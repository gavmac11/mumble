// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifdef USE_SCREEN_SHARING

#	include "ScreenPickerDialog.h"

#	include "CaptureSourceLister.h"
#	include "VideoViewUi.h"
#	include <QtWidgets/QTabBar>
#	include <QtWidgets/QStyle>

#	include <QtWidgets/QDialogButtonBox>
#	include <QtWidgets/QLabel>
#	include <QtWidgets/QListWidget>
#	include <QtWidgets/QPushButton>
#	include <QtWidgets/QVBoxLayout>

ScreenPickerDialog::ScreenPickerDialog(QWidget *parent) : QDialog(parent) {
	setWindowTitle(tr("Choose what to share"));
	setMinimumSize(640, 480);
	resize(780, 560);
	VideoViewUi::style(this);
	m_tabs = new QTabBar(this);
	m_tabs->addTab(tr("Screens"));
	m_tabs->addTab(tr("Windows"));
	m_tabs->addTab(tr("Camera"));
	m_tabs->setExpanding(false);
	m_tabs->setAccessibleName(tr("Capture source type"));

	m_list = new QListWidget(this);
	m_list->setViewMode(QListView::IconMode);
	m_list->setIconSize(QSize(192, 108));
	m_list->setGridSize(QSize(228, 164));
	m_list->setAccessibleName(tr("Available capture sources"));
	m_list->setResizeMode(QListView::Adjust);
	m_list->setSpacing(8);
	m_list->setWordWrap(true);
	m_list->setMovement(QListView::Static);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	connect(m_list, &QListWidget::itemDoubleClicked, this, &ScreenPickerDialog::onItemDoubleClicked);

	auto *heading = new QLabel(tr("Choose what to share"), this);
	heading->setObjectName(QStringLiteral("videoHeading"));
	m_hint = new QLabel(this);
	m_hint->setObjectName(QStringLiteral("videoHint"));
	m_hint->setWordWrap(true);
	m_selection = new QLabel(this);
	m_selection->setTextFormat(Qt::PlainText);
	m_selection->setWordWrap(true);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_shareButton = buttons->button(QDialogButtonBox::Ok);
	m_shareButton->setObjectName(QStringLiteral("shareButton"));
	m_shareButton->setText(tr("Share"));
	m_shareButton->setEnabled(false);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		if (m_list->currentItem())
			accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { updateSelection(); });
	connect(m_tabs, &QTabBar::currentChanged, this, [this](int) { updateSources(); });

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 24, 24, 20);
	layout->setSpacing(16);
	layout->addWidget(heading);
	layout->addWidget(m_tabs);
	layout->addWidget(m_hint);
	layout->addWidget(m_list, 1);
	layout->addWidget(m_selection);
	layout->addWidget(buttons);

	m_sources = listCaptureSources();
	updateSources();
}

void ScreenPickerDialog::updateSources() {
	m_list->clear();
	const int tab = m_tabs->currentIndex();
	m_hint->setText(tab == 0   ? tr("Everything on this screen will be visible to your channel.")
					: tab == 1 ? tr("Share a single window to keep the rest of your desktop private.")
							   : tr("Choose a camera. A live self-preview opens when sharing starts."));
	for (int index = 0; index < m_sources.size(); ++index) {
		const CaptureSource &src = m_sources.at(index);
		const bool native        = src.type == CaptureSource::Type::NativePicker;
		const bool matches       = tab == 0   ? src.type == CaptureSource::Type::EntireScreen || native
								   : tab == 1 ? src.type == CaptureSource::Type::Window || native
											  : src.type == CaptureSource::Type::Webcam;
		if (!matches)
			continue;
		QIcon icon = src.thumbnail.isNull()
						 ? style()->standardIcon(tab == 2 ? QStyle::SP_MediaPlay : QStyle::SP_ComputerIcon)
						 : QIcon(src.thumbnail);
		auto *item = new QListWidgetItem(icon, src.displayName, m_list);
		item->setData(Qt::UserRole, index);
		item->setToolTip(src.displayName);
	}
	updateSelection();
}

void ScreenPickerDialog::updateSelection() {
	const auto *item = m_list->currentItem();
	m_shareButton->setEnabled(item != nullptr);
	if (!item) {
		m_selection->setText(m_list->count() ? tr("Select a source to continue.")
											 : tr("No sources available here. Try another tab."));
		m_shareButton->setText(tr("Share"));
		return;
	}
	const CaptureSource &source = m_sources.at(item->data(Qt::UserRole).toInt());
	m_selection->setText(tr("Selected: %1").arg(source.displayName));
	m_shareButton->setText(source.type == CaptureSource::Type::NativePicker ? tr("Continue…")
						   : source.type == CaptureSource::Type::Webcam     ? tr("Start camera")
																			: tr("Share"));
}

CaptureSource ScreenPickerDialog::selectedSource() const {
	const auto *item = m_list->currentItem();
	const int row    = item ? item->data(Qt::UserRole).toInt() : -1;
	if (row >= 0 && row < m_sources.size())
		return m_sources.at(row);
	return {};
}

void ScreenPickerDialog::onItemDoubleClicked(QListWidgetItem *item) {
	if (item)
		accept();
}

#endif // USE_SCREEN_SHARING
