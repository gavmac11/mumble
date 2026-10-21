// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHATWINDOW_H_
#define MUMBLE_MUMBLE_CHATWINDOW_H_

#include <QtGui/QKeyEvent>
#include <QtWidgets/QDialog>

/// Keep normal chat keyboard behavior while retaining QDialog's explicit close notification.
class ChatWindow : public QDialog {
public:
	explicit ChatWindow(QWidget *parent = nullptr) : QDialog(parent, Qt::Window) {}

protected:
	void keyPressEvent(QKeyEvent *event) override {
		// Escape in the chat input must not reject the dialog and return chat to its docks.
		if (event->key() == Qt::Key_Escape) {
			event->accept();
			return;
		}
		QDialog::keyPressEvent(event);
	}
};

#endif // MUMBLE_MUMBLE_CHATWINDOW_H_
