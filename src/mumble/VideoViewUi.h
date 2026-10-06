// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOVIEWUI_H_
#define MUMBLE_MUMBLE_VIDEOVIEWUI_H_

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QShortcut>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>

namespace VideoViewUi {

inline void style(QWidget *widget) {
	// Leave colours, fonts, focus indicators and control borders to the active theme.
	// Only spacing and emphasis are shared between these windows.
	widget->setStyleSheet(QStringLiteral("QLabel#videoHeading, QLabel#sharingStatus { font-weight: 600; }"
										 "QPushButton { padding: 8px 14px; }"
										 "QPushButton#stopSharing { font-weight: 600; }"
										 "QListWidget::item { padding: 10px; }"
										 "QTabBar::tab { padding: 10px 22px; }"));
}

/// Tracks native window-state changes as well as changes initiated by our controls.
class FullscreenButton : public QPushButton {
	Q_DECLARE_TR_FUNCTIONS(VideoViewUi)

public:
	explicit FullscreenButton(QDialog *window) : QPushButton(window), m_window(window) {
		setAutoDefault(false);
		const QKeySequence shortcut(QKeySequence::FullScreen);
		setToolTip(
			tr("Toggle full screen (%1). Escape exits full screen.").arg(shortcut.toString(QKeySequence::NativeText)));
		connect(this, &QPushButton::clicked, this, &FullscreenButton::toggle);
		auto *fullscreen = new QShortcut(shortcut, window);
		fullscreen->setKeys(QKeySequence::keyBindings(QKeySequence::FullScreen));
		connect(fullscreen, &QShortcut::activated, this, &FullscreenButton::toggle);
		auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), window);
		connect(escape, &QShortcut::activated, this, [this]() {
			if (m_window->isFullScreen())
				toggle();
			else
				m_window->close();
		});
		window->installEventFilter(this);
		updateText();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == m_window && event->type() == QEvent::WindowStateChange)
			updateText();
		return QPushButton::eventFilter(watched, event);
	}

private:
	void toggle() { m_window->setWindowState(m_window->windowState() ^ Qt::WindowFullScreen); }

	void updateText() { setText(m_window->isFullScreen() ? tr("Exit full screen") : tr("Full screen")); }

	QDialog *m_window;
};

/// Called once per window to install its full-screen control and shortcuts.
inline QPushButton *fullscreenButton(QDialog *window) {
	return new FullscreenButton(window);
}

} // namespace VideoViewUi

#endif // MUMBLE_MUMBLE_VIDEOVIEWUI_H_
