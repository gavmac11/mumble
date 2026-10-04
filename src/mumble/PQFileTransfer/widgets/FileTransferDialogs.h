// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// File-transfer dialogs: safety-number verification (mandatory on first
// contact, PQShield v2 §5) and the pre-send dialog.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_

#include <QByteArray>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

class QCheckBox;
class QDialogButtonBox;
class QVBoxLayout;

/// Safety-number verification. Modal; `verified()` tells whether the user
/// confirmed the numbers match out-of-band (QR / in person / voice).
class SafetyNumberDialog : public QDialog {
	Q_OBJECT
public:
	SafetyNumberDialog(const QString &peerName, const QString &safetyDigits, const QByteArray &qrPayload,
					   QWidget *parent = nullptr);

	bool verified() const { return m_verified; }

private:
	bool m_verified = false;
};

/// Simple password entry with confirmation and a strength hint.
class FilePasswordWidget : public QWidget {
	Q_OBJECT
public:
	explicit FilePasswordWidget(bool withConfirmation, QWidget *parent = nullptr);

	/// The entered password (empty when too weak / mismatching).
	QByteArray password() const;
	bool isValid() const;

private slots:
	void updateStrength();

private:
	QLineEdit *m_password   = nullptr;
	QLineEdit *m_confirm	= nullptr;
	QLabel *m_strengthLabel = nullptr;
	QCheckBox *m_show		= nullptr;
};

/// Pre-send dialog: recipients with trust states, optional password.
class FileSendDialog : public QDialog {
	Q_OBJECT
public:
	struct Recipient {
		unsigned int session = 0;
		QString name;
		int trustState = 0;   // PQFT::TrustState
	};

	FileSendDialog(const QString &fileName, quint64 fileSize, const QList< Recipient > &recipients,
				   QWidget *parent = nullptr);

	QList< unsigned int > selectedSessions() const;
	bool passwordEnabled() const;
	QByteArray password() const;

private:
	QListWidget *m_recipients  = nullptr;
	QCheckBox *m_encrypt	   = nullptr;
	FilePasswordWidget *m_passwordWidget = nullptr;
	QDialogButtonBox *m_buttons = nullptr;

	void updateOkState();
};

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_
