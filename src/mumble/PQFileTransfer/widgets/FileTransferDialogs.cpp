// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE/>.

#include "PQFileTransfer/widgets/FileTransferDialogs.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/identity/PeerTrustStore.h"

#include <qrcodegen.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QImage renderQr(const QByteArray &payload) {
	const QByteArray bytes = payload.isEmpty() ? QByteArray("mumble-file-transfer") : payload;
	const auto *begin      = reinterpret_cast< const std::uint8_t * >(bytes.constData());
	const std::vector< std::uint8_t > content(begin, begin + bytes.size());
	qrcodegen::QrCode qr = qrcodegen::QrCode::encodeBinary(content, qrcodegen::QrCode::Ecc::MEDIUM);

	const int border = 4;
	const int size   = qr.getSize() + border * 2;
	QImage image(size, size, QImage::Format_RGB32);
	image.fill(Qt::white);
	for (int y = 0; y < qr.getSize(); ++y) {
		for (int x = 0; x < qr.getSize(); ++x) {
			if (qr.getModule(x, y)) {
				image.setPixel(x + border, y + border, qRgb(0, 0, 0));
			}
		}
	}
	return image;
}

QString strengthHint(const QString &password) {
	if (password.size() < PQFT::MinPasswordLength) {
		return FileSendDialog::tr("Too short (minimum 8 characters)");
	}
	int classes = 0;
	if (password.contains(QRegularExpression(QStringLiteral("[a-z]"))))
		++classes;
	if (password.contains(QRegularExpression(QStringLiteral("[A-Z]"))))
		++classes;
	if (password.contains(QRegularExpression(QStringLiteral("[0-9]"))))
		++classes;
	if (password.contains(QRegularExpression(QStringLiteral("[^a-zA-Z0-9]"))))
		++classes;
	const double entropy = static_cast< double >(password.size()) * (classes >= 3 ? 5.5 : 4.0);
	if (entropy >= 80) {
		return FileSendDialog::tr("Strong — a long passphrase is even better");
	} else if (entropy >= 55) {
		return FileSendDialog::tr("Moderate");
	}
	return FileSendDialog::tr("Weak — prefer a long passphrase");
}

QLabel *wrappedLabel(const QString &text, QWidget *parent) {
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	return label;
}
} // namespace

// --- SafetyNumberDialog ------------------------------------------------------

SafetyNumberDialog::SafetyNumberDialog(const QString &peerName, const QString &safetyDigits,
									   const QByteArray &qrPayload, QWidget *parent, Purpose purpose)
	: QDialog(parent), m_verified(false) {
	const QString name = peerName.toHtmlEscaped();
	switch (purpose) {
		case Purpose::Sending:
			setWindowTitle(tr("Verify recipient before sending"));
			break;
		case Purpose::Receiving:
			setWindowTitle(tr("Verify sender before receiving"));
			break;
		case Purpose::Manual:
			setWindowTitle(tr("Verify %1").arg(peerName));
			break;
	}
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	QString intro;
	switch (purpose) {
		case Purpose::Sending:
			intro = tr("Your settings ask you to verify new contacts first. Compare this safety number with the one "
					   "<b>%1</b> sees before the file is sent.")
						.arg(name);
			break;
		case Purpose::Receiving:
			intro = tr("Your settings ask you to verify new contacts first. Compare this safety number with the one "
					   "<b>%1</b> sees before accepting their file.")
						.arg(name);
			break;
		case Purpose::Manual:
			intro = tr("Compare this safety number with the one <b>%1</b> sees (they open the same dialog from your "
					   "name). Scan the QR code or read the digits aloud — they must match exactly.")
						.arg(name);
			break;
	}
	auto *introLabel = wrappedLabel(intro, this);
	introLabel->setTextFormat(Qt::RichText);
	layout->addWidget(introLabel);

	const QImage qr = renderQr(qrPayload);
	auto *qrLabel   = new QLabel(this);
	qrLabel->setAlignment(Qt::AlignCenter);
	qrLabel->setPixmap(QPixmap::fromImage(qr).scaled(220, 220, Qt::KeepAspectRatio, Qt::FastTransformation));
	layout->addWidget(qrLabel);

	auto *digits = new QLabel(safetyDigits, this);
	QFont mono   = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	mono.setPointSizeF(mono.pointSizeF() * 1.4);
	mono.setBold(true);
	digits->setFont(mono);
	digits->setAlignment(Qt::AlignCenter);
	digits->setWordWrap(true);
	digits->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(digits);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, this);
	buttons->button(QDialogButtonBox::Yes)->setText(tr("They match — &verify"));
	buttons->button(QDialogButtonBox::No)->setText(purpose == Purpose::Manual ? tr("&Cancel") : tr("&Not now"));
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		m_verified = true;
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

// --- FilePasswordWidget ------------------------------------------------------

FilePasswordWidget::FilePasswordWidget(bool withConfirmation, QWidget *parent) : QWidget(parent) {
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	m_password = new QLineEdit(this);
	m_password->setEchoMode(QLineEdit::Password);
	m_password->setPlaceholderText(tr("Password (minimum 8 characters)"));
	layout->addWidget(m_password);

	if (withConfirmation) {
		m_confirm = new QLineEdit(this);
		m_confirm->setEchoMode(QLineEdit::Password);
		m_confirm->setPlaceholderText(tr("Repeat password"));
		layout->addWidget(m_confirm);
	}

	m_show = new QCheckBox(tr("Show password"), this);
	layout->addWidget(m_show);

	m_strengthLabel = new QLabel(this);
	m_strengthLabel->setStyleSheet(QStringLiteral("color: gray;"));
	layout->addWidget(m_strengthLabel);

	connect(m_password, &QLineEdit::textChanged, this, &FilePasswordWidget::updateStrength);
	if (m_confirm) {
		connect(m_confirm, &QLineEdit::textChanged, this, &FilePasswordWidget::updateStrength);
	}
	connect(m_show, &QCheckBox::toggled, this, [this](bool shown) {
		m_password->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		if (m_confirm) {
			m_confirm->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		}
	});

	updateStrength();
}

void FilePasswordWidget::updateStrength() {
	const QString text = m_password->text();
	QString hint       = strengthHint(text);
	if (m_confirm && !text.isEmpty() && m_confirm->text() != text) {
		hint += tr(" — passwords do not match");
	}
	m_strengthLabel->setText(hint);
	emit changed();
}

QByteArray FilePasswordWidget::password() const {
	return isValid() ? m_password->text().toUtf8() : QByteArray();
}

bool FilePasswordWidget::isValid() const {
	if (m_password->text().size() < PQFT::MinPasswordLength) {
		return false;
	}
	if (m_confirm && m_confirm->text() != m_password->text()) {
		return false;
	}
	return true;
}

void FilePasswordWidget::focusPassword() {
	m_password->setFocus(Qt::OtherFocusReason);
}

// --- FileSendDialog ------------------------------------------------------------

FileSendDialog::FileSendDialog(const QStringList &fileNames, quint64 totalSize, const QList< Recipient > &recipients,
							   bool verificationRequired, QWidget *parent)
	: QDialog(parent) {
	const QString size = QLocale().formattedDataSize(static_cast< qint64 >(totalSize));
	if (fileNames.size() == 1) {
		setWindowTitle(tr("Send file"));
	} else {
		setWindowTitle(tr("Send %n file(s)", "", static_cast< int >(fileNames.size())));
	}
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	QString summary;
	if (fileNames.size() == 1) {
		summary = tr("<b>%1</b> · %2").arg(fileNames.first().toHtmlEscaped(), size);
	} else {
		QStringList shown;
		for (qsizetype i = 0; i < fileNames.size() && i < 3; ++i)
			shown << QStringLiteral("<b>%1</b>").arg(fileNames.at(i).toHtmlEscaped());
		summary = shown.join(QStringLiteral(", "));
		if (fileNames.size() > 3)
			summary += tr(" and %n more", "", static_cast< int >(fileNames.size() - 3));
		summary += QStringLiteral(" · ") + size;
	}
	auto *fileLabel = wrappedLabel(summary, this);
	fileLabel->setTextFormat(Qt::RichText);
	layout->addWidget(fileLabel);

	layout->addWidget(new QLabel(tr("Send to:"), this));

	m_recipients = new QListWidget(this);
	for (const Recipient &recipient : recipients) {
		auto *item = new QListWidgetItem(m_recipients);
		QString trust;
		switch (static_cast< PQFT::TrustState >(recipient.trustState)) {
			case PQFT::TrustState::Verified:
				trust = tr("verified ✓");
				break;
			case PQFT::TrustState::Pinned:
				break;
			case PQFT::TrustState::StorageError:
				trust = tr("can't read their saved key — unavailable");
				break;
			case PQFT::TrustState::Changed:
				trust = tr("their key changed — blocked");
				break;
			case PQFT::TrustState::NewPeer:
			default:
				if (verificationRequired)
					trust = tr("first transfer — you'll compare safety numbers first");
				break;
		}
		item->setText(trust.isEmpty() ? recipient.name : tr("%1 — %2").arg(recipient.name, trust));
		item->setData(Qt::UserRole, recipient.session);
		item->setData(Qt::UserRole + 1, recipient.trustState);
		if (recipient.trustState == static_cast< int >(PQFT::TrustState::Changed)
			|| recipient.trustState == static_cast< int >(PQFT::TrustState::StorageError)) {
			// Hard exclusion: a peer whose pinned identity changed is never a
			// valid recipient. Gray the entry out and make it inert (the
			// engine refuses the handshake anyway — this is UX only).
			item->setFlags((item->flags() & ~Qt::ItemIsUserCheckable) & ~Qt::ItemIsEnabled);
			item->setCheckState(Qt::Unchecked);
			item->setForeground(QBrush(QColor(0xe8, 0x74, 0x6f)));
		} else {
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Checked);
		}
	}
	// Keep short recipient lists compact instead of showing a tall empty box.
	const int rows = static_cast< int >(qBound(qsizetype(1), recipients.size(), qsizetype(6)));
	m_recipients->setMaximumHeight(rows * m_recipients->fontMetrics().height() * 3 / 2 + 2 * m_recipients->frameWidth()
								   + 6);
	layout->addWidget(m_recipients);

	m_encrypt = new QCheckBox(tr("Protect with a password"), this);
	m_encrypt->setToolTip(
		tr("Adds a second layer of encryption. Recipients need the password to open the file — share it separately."));
	layout->addWidget(m_encrypt);

	m_passwordWidget = new FilePasswordWidget(true, this);
	m_passwordWidget->setVisible(false);
	layout->addWidget(m_passwordWidget);
	connect(m_encrypt, &QCheckBox::toggled, this, [this](bool checked) {
		m_passwordWidget->setVisible(checked);
		if (checked)
			m_passwordWidget->focusPassword();
		adjustSize();
		updateOkState();
	});

	m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	QPushButton *send = m_buttons->button(QDialogButtonBox::Ok);
	send->setText(tr("&Send"));
	send->setDefault(true);
	connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(m_buttons);

	connect(m_recipients, &QListWidget::itemChanged, this, [this]() { updateOkState(); });
	connect(m_passwordWidget, &FilePasswordWidget::changed, this, [this]() { updateOkState(); });

	updateOkState();
	send->setFocus(Qt::OtherFocusReason);
}

QList< unsigned int > FileSendDialog::selectedSessions() const {
	QList< unsigned int > sessions;
	for (int i = 0; i < m_recipients->count(); ++i) {
		const QListWidgetItem *item = m_recipients->item(i);
		// Identity-changed peers are hard-excluded even if their check state
		// were somehow toggled
		if (item->data(Qt::UserRole + 1).toInt() == static_cast< int >(PQFT::TrustState::Changed)) {
			continue;
		}
		if (item->checkState() == Qt::Checked) {
			sessions.append(item->data(Qt::UserRole).toUInt());
		}
	}
	return sessions;
}

bool FileSendDialog::passwordEnabled() const {
	return m_encrypt->isChecked() && m_passwordWidget->isValid();
}

QByteArray FileSendDialog::password() const {
	return passwordEnabled() ? m_passwordWidget->password() : QByteArray();
}

void FileSendDialog::updateOkState() {
	bool ok = !selectedSessions().isEmpty();
	if (m_encrypt->isChecked() && !m_passwordWidget->isValid()) {
		ok = false;
	}
	m_buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
}

// --- FileIdentityUnlockDialog ----------------------------------------------------

FileIdentityUnlockDialog::FileIdentityUnlockDialog(std::function< bool(const QString &) > tryUnlock,
												   bool offerAutoUnlock, QWidget *parent)
	: QDialog(parent), m_tryUnlock(std::move(tryUnlock)) {
	setWindowTitle(tr("Unlock file sharing"));
	setModal(true);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(wrappedLabel(tr("Your file-sharing key is protected by a passphrase. Enter it to send and "
									  "receive files on this server."),
								   this));

	m_passphrase = new QLineEdit(this);
	m_passphrase->setEchoMode(QLineEdit::Password);
	m_passphrase->setPlaceholderText(tr("Passphrase"));
	layout->addWidget(m_passphrase);

	m_error = new QLabel(this);
	m_error->setStyleSheet(QStringLiteral("color: #e8746f;"));
	m_error->setVisible(false);
	layout->addWidget(m_error);

	m_autoUnlock = new QCheckBox(tr("Unlock automatically on this computer from now on"), this);
	m_autoUnlock->setToolTip(tr("Stops asking for the passphrase. The key is then protected by your computer account, "
								"like your Mumble certificate."));
	m_autoUnlock->setChecked(offerAutoUnlock);
	layout->addWidget(m_autoUnlock);

	auto *buttons       = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	QPushButton *unlock = buttons->button(QDialogButtonBox::Ok);
	unlock->setText(tr("&Unlock"));
	unlock->setDefault(true);
	buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Skip"));
	QPushButton *forgot = buttons->addButton(tr("&Forgot passphrase?"), QDialogButtonBox::ResetRole);
	forgot->setFlat(true);
	layout->addWidget(buttons);

	unlock->setEnabled(false);
	connect(m_passphrase, &QLineEdit::textChanged, this,
			[unlock](const QString &text) { unlock->setEnabled(!text.isEmpty()); });
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		if (m_passphrase->text().isEmpty())
			return;
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
		const bool unlocked = m_tryUnlock && m_tryUnlock(m_passphrase->text());
		QApplication::restoreOverrideCursor();
		m_passphrase->clear();
		if (unlocked) {
			m_outcome = Outcome::Unlocked;
			accept();
			return;
		}
		m_error->setText(tr("Wrong passphrase. Try again."));
		m_error->setVisible(true);
		m_passphrase->setFocus(Qt::OtherFocusReason);
	});
	connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
		m_outcome = Outcome::Skipped;
		reject();
	});
	connect(forgot, &QPushButton::clicked, this, [this]() {
		QMessageBox confirm(QMessageBox::Warning, tr("Start over?"),
							tr("Create a new file-sharing key? People you have shared files with will see that your "
							   "key changed. They need to reset it (right-click your name → Reset file identity) "
							   "before you can exchange files again."),
							QMessageBox::Yes | QMessageBox::No, this);
		confirm.setDefaultButton(QMessageBox::No);
		if (confirm.exec() == QMessageBox::Yes) {
			m_outcome = Outcome::ResetRequested;
			accept();
		}
	});
	m_passphrase->setFocus(Qt::OtherFocusReason);
}

bool FileIdentityUnlockDialog::autoUnlockChosen() const {
	return m_autoUnlock->isChecked();
}

// --- FileIdentityPassphraseDialog ------------------------------------------------

FileIdentityPassphraseDialog::FileIdentityPassphraseDialog(QWidget *parent) : QDialog(parent) {
	setWindowTitle(tr("Set a passphrase"));
	setModal(true);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(wrappedLabel(tr("Protect your file-sharing key with a passphrase. You will need to enter it "
									  "each time you connect to a server. If you forget it, you can start over with "
									  "a new key."),
								   this));
	m_passwordWidget = new FilePasswordWidget(true, this);
	layout->addWidget(m_passwordWidget);

	auto *buttons   = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
	ok->setText(tr("&Set passphrase"));
	ok->setEnabled(false);
	connect(m_passwordWidget, &FilePasswordWidget::changed, this,
			[this, ok]() { ok->setEnabled(m_passwordWidget->isValid()); });
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
	m_passwordWidget->focusPassword();
}

QString FileIdentityPassphraseDialog::passphrase() const {
	return QString::fromUtf8(m_passwordWidget->password());
}

// --- FilePasswordBar -------------------------------------------------------------

FilePasswordBar::FilePasswordBar(QWidget *parent) : QFrame(parent) {
	setObjectName(QStringLiteral("qfFilePasswordBar"));
	setFrameShape(QFrame::StyledPanel);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(6, 3, 3, 3);
	layout->setSpacing(6);

	m_label = new QLabel(this);
	m_label->setTextFormat(Qt::PlainText);
	m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	m_label->setMinimumWidth(80);
	layout->addWidget(m_label, 1);

	m_password = new QLineEdit(this);
	m_password->setEchoMode(QLineEdit::Password);
	m_password->setPlaceholderText(tr("File password"));
	m_password->setAccessibleName(tr("File password"));
	m_password->setMinimumWidth(120);
	layout->addWidget(m_password);

	m_unlock = new QPushButton(tr("Unlock"), this);
	m_unlock->setEnabled(false);
	layout->addWidget(m_unlock);

	m_close = new QToolButton(this);
	m_close->setAutoRaise(true);
	m_close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
	m_close->setToolTip(tr("Hide (click the file in the chat to enter the password later)"));
	m_close->setAccessibleName(tr("Hide password prompt"));
	layout->addWidget(m_close);

	connect(m_password, &QLineEdit::textChanged, this, [this](const QString &text) {
		m_unlock->setEnabled(!text.isEmpty() && !m_pending.isEmpty() && !m_pending.first().checking);
	});
	connect(m_password, &QLineEdit::returnPressed, this, &FilePasswordBar::submit);
	connect(m_unlock, &QPushButton::clicked, this, &FilePasswordBar::submit);
	connect(m_close, &QToolButton::clicked, this, [this]() {
		if (!m_pending.isEmpty())
			m_pending.removeFirst();
		showCurrent();
	});

	setVisible(false);
}

int FilePasswordBar::indexOf(const QByteArray &transferId) const {
	for (int i = 0; i < m_pending.size(); ++i)
		if (m_pending.at(i).transferId == transferId)
			return i;
	return -1;
}

bool FilePasswordBar::isWaitingFor(const QByteArray &transferId) const {
	return indexOf(transferId) >= 0;
}

void FilePasswordBar::request(const QByteArray &transferId, const QString &fileName, const QString &senderName,
							  bool retry) {
	const int index = indexOf(transferId);
	if (index >= 0) {
		Pending &pending = m_pending[index];
		pending.retry    = retry;
		pending.checking = false;
		if (!fileName.isEmpty())
			pending.fileName = fileName;
		if (!senderName.isEmpty())
			pending.senderName = senderName;
	} else {
		Pending pending;
		pending.transferId = transferId;
		pending.fileName   = fileName;
		pending.senderName = senderName;
		pending.retry      = retry;
		m_pending.append(pending);
	}
	showCurrent();
}

void FilePasswordBar::dismiss(const QByteArray &transferId) {
	const int index = indexOf(transferId);
	if (index < 0)
		return;
	m_pending.removeAt(index);
	if (index == 0)
		m_password->clear();
	showCurrent();
}

void FilePasswordBar::focusTransfer(const QByteArray &transferId) {
	const int index = indexOf(transferId);
	if (index < 0)
		return;
	if (index > 0) {
		m_pending.move(index, 0);
		m_password->clear();
	}
	showCurrent();
	m_password->setFocus(Qt::OtherFocusReason);
}

void FilePasswordBar::clear() {
	m_pending.clear();
	m_password->clear();
	showCurrent();
}

void FilePasswordBar::showCurrent() {
	if (m_pending.isEmpty()) {
		m_password->clear();
		setVisible(false);
		return;
	}
	const Pending &current = m_pending.first();
	QString text;
	if (current.checking) {
		text = tr("Checking password…");
	} else if (current.retry) {
		text = tr("Wrong password for \"%1\". Try again.").arg(current.fileName);
	} else if (current.senderName.isEmpty()) {
		text = tr("\"%1\" is password protected.").arg(current.fileName);
	} else {
		text = tr("\"%1\" from %2 is password protected.").arg(current.fileName, current.senderName);
	}
	if (m_pending.size() > 1)
		text += QStringLiteral(" ") + tr("(%n more waiting)", "", static_cast< int >(m_pending.size() - 1));
	m_label->setText(text);
	m_label->setToolTip(text);
	m_label->setStyleSheet(current.retry && !current.checking ? QStringLiteral("color: #e8746f;") : QString());
	m_password->setEnabled(!current.checking);
	m_unlock->setEnabled(!current.checking && !m_password->text().isEmpty());
	setVisible(true);
}

void FilePasswordBar::submit() {
	if (m_pending.isEmpty() || m_pending.first().checking || m_password->text().isEmpty())
		return;
	Pending &current    = m_pending.first();
	current.checking    = true;
	QByteArray password = m_password->text().toUtf8();
	m_password->clear();
	const QByteArray transferId = current.transferId;
	showCurrent();
	emit passwordEntered(transferId, password);
	PQFT::zeroize(password);
}
