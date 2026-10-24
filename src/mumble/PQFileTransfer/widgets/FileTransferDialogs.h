// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// File-transfer dialogs and widgets: safety-number verification (optional by
// default, see SECURITY.md), the pre-send dialog, identity passphrase prompts
// and the inline password bar for incoming password-protected files.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_

#include <QByteArray>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>

#include <functional>

class QCheckBox;
class QDialogButtonBox;
class QToolButton;
class QVBoxLayout;

/// Safety-number verification. Modal; `verified()` tells whether the user
/// confirmed the numbers match out-of-band (QR / in person / voice).
class SafetyNumberDialog : public QDialog {
	Q_OBJECT
public:
	enum class Purpose {
		/// A transfer from this peer waits for verification.
		Receiving,
		/// A transfer to this peer waits for verification.
		Sending,
		/// The user chose to verify a known contact (user menu).
		Manual,
	};

	SafetyNumberDialog(const QString &peerName, const QString &safetyDigits, const QByteArray &qrPayload,
					   QWidget *parent = nullptr, Purpose purpose = Purpose::Receiving);

	bool verified() const { return m_verified; }

private:
	bool m_verified = false;
};

/// Simple password entry with optional confirmation and a strength hint.
class FilePasswordWidget : public QWidget {
	Q_OBJECT
public:
	explicit FilePasswordWidget(bool withConfirmation, QWidget *parent = nullptr);

	/// The entered password (empty when too weak / mismatching).
	QByteArray password() const;
	bool isValid() const;
	/// Move keyboard focus to the first password field.
	void focusPassword();

signals:
	void changed();

private slots:
	void updateStrength();

private:
	QLineEdit *m_password   = nullptr;
	QLineEdit *m_confirm    = nullptr;
	QLabel *m_strengthLabel = nullptr;
	QCheckBox *m_show       = nullptr;
};

/// Pre-send dialog: one or more files, recipients with trust states and an
/// optional password.
class FileSendDialog : public QDialog {
	Q_OBJECT
public:
	struct Recipient {
		unsigned int session = 0;
		QString name;
		int trustState = 0; // PQFT::TrustState
	};

	/// `verificationRequired` mirrors the setting: new contacts then need a
	/// safety-number check before their first transfer.
	FileSendDialog(const QStringList &fileNames, quint64 totalSize, const QList< Recipient > &recipients,
				   bool verificationRequired, QWidget *parent = nullptr);

	QList< unsigned int > selectedSessions() const;
	bool passwordEnabled() const;
	QByteArray password() const;

private:
	QListWidget *m_recipients            = nullptr;
	QCheckBox *m_encrypt                 = nullptr;
	FilePasswordWidget *m_passwordWidget = nullptr;
	QDialogButtonBox *m_buttons          = nullptr;

	void updateOkState();
};

/// Unlock prompt for a passphrase-protected file-sharing identity. Wrong
/// passphrases are reported inline; the dialog stays open until the identity
/// unlocks, the user skips, or asks to start over with a new identity.
class FileIdentityUnlockDialog : public QDialog {
	Q_OBJECT
public:
	enum class Outcome { Skipped, Unlocked, ResetRequested };

	/// `tryUnlock` returns true when the passphrase unlocked the identity.
	/// `offerAutoUnlock` pre-checks "unlock automatically from now on".
	FileIdentityUnlockDialog(std::function< bool(const QString &) > tryUnlock, bool offerAutoUnlock,
							 QWidget *parent = nullptr);

	Outcome outcome() const { return m_outcome; }
	/// The user wants the identity switched to automatic (device) unlocking.
	bool autoUnlockChosen() const;

private:
	std::function< bool(const QString &) > m_tryUnlock;
	QLineEdit *m_passphrase = nullptr;
	QLabel *m_error         = nullptr;
	QCheckBox *m_autoUnlock = nullptr;
	Outcome m_outcome       = Outcome::Skipped;
};

/// Choose a passphrase for the file-sharing identity (settings page).
class FileIdentityPassphraseDialog : public QDialog {
	Q_OBJECT
public:
	explicit FileIdentityPassphraseDialog(QWidget *parent = nullptr);
	QString passphrase() const;

private:
	FilePasswordWidget *m_passwordWidget = nullptr;
};

/// Inline, non-modal password entry shown above the chat bar while incoming
/// files wait for their password. Shows one file at a time.
class FilePasswordBar : public QFrame {
	Q_OBJECT
public:
	explicit FilePasswordBar(QWidget *parent = nullptr);

	/// Show (or refresh) the prompt for a transfer waiting for its password.
	/// `retry` reports that the previous attempt was wrong.
	void request(const QByteArray &transferId, const QString &fileName, const QString &senderName, bool retry);
	/// The transfer no longer waits for a password.
	void dismiss(const QByteArray &transferId);
	/// Bring a waiting transfer to the front and focus the password field.
	void focusTransfer(const QByteArray &transferId);
	bool isWaitingFor(const QByteArray &transferId) const;
	void clear();

signals:
	void passwordEntered(const QByteArray &transferId, const QByteArray &password);

private:
	struct Pending {
		QByteArray transferId;
		QString fileName;
		QString senderName;
		bool retry    = false;
		bool checking = false;
	};
	QList< Pending > m_pending;
	QLabel *m_label       = nullptr;
	QLineEdit *m_password = nullptr;
	QPushButton *m_unlock = nullptr;
	QToolButton *m_close  = nullptr;

	int indexOf(const QByteArray &transferId) const;
	void showCurrent();
	void submit();
};

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_WIDGETS_FILETRANSFERDIALOGS_H_
