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
#include <vector>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QBrush>
#include <QColor>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {
QImage renderQr(const QByteArray &payload) {
	const QByteArray bytes = payload.isEmpty() ? QByteArray("mumble-file-transfer") : payload;
	const auto *begin = reinterpret_cast< const std::uint8_t * >(bytes.constData());
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
} // namespace

SafetyNumberDialog::SafetyNumberDialog(const QString &peerName, const QString &safetyDigits,
									   const QByteArray &qrPayload, QWidget *parent)
	: QDialog(parent), m_verified(false) {
	setWindowTitle(tr("Verify file-transfer identity"));
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	auto *intro = new QLabel(
		tr("First file transfer with <b>%1</b>. Compare the safety number below with the one on their "
		   "screen — scan the QR code or read the digits aloud. They must match exactly.")
			.arg(peerName.toHtmlEscaped()),
		this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	const QImage qr = renderQr(qrPayload);
	auto *qrLabel   = new QLabel(this);
	qrLabel->setAlignment(Qt::AlignCenter);
	qrLabel->setPixmap(QPixmap::fromImage(qr).scaled(220, 220, Qt::KeepAspectRatio,
													 Qt::FastTransformation));
	layout->addWidget(qrLabel);

	auto *digits = new QLabel(safetyDigits, this);
	QFont mono   = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	mono.setPointSizeF(mono.pointSizeF() * 1.4);
	mono.setBold(true);
	digits->setFont(mono);
	digits->setAlignment(Qt::AlignCenter);
	digits->setWordWrap(true);
	layout->addWidget(digits);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, this);
	buttons->button(QDialogButtonBox::Yes)->setText(tr("They match — &verify"));
	buttons->button(QDialogButtonBox::No)->setText(tr("&Not now"));
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
	QString hint		  = strengthHint(text);
	if (m_confirm && !text.isEmpty() && m_confirm->text() != text) {
		hint += tr(" — passwords do not match");
	}
	m_strengthLabel->setText(hint);
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

// --- FileSendDialog ------------------------------------------------------------

FileSendDialog::FileSendDialog(const QString &fileName, quint64 fileSize,
							   const QList< Recipient > &recipients, QWidget *parent)
	: QDialog(parent) {
	setWindowTitle(tr("Send file to channel"));
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	auto *fileLabel = new QLabel(tr("<b>%1</b> (%2 bytes)").arg(fileName.toHtmlEscaped()).arg(fileSize),
								this);
	fileLabel->setWordWrap(true);
	layout->addWidget(fileLabel);

	m_recipients = new QListWidget(this);
	for (const Recipient &recipient : recipients) {
		auto *item = new QListWidgetItem(m_recipients);
		QString trust;
		switch (static_cast< PQFT::TrustState >(recipient.trustState)) {
			case PQFT::TrustState::Verified:
				trust = tr("verified");
				break;
			case PQFT::TrustState::Pinned:
				trust = tr("pinned (not verified yet)");
				break;
			case PQFT::TrustState::Changed:
				trust = tr("IDENTITY CHANGED — blocked");
				break;
			case PQFT::TrustState::NewPeer:
			default:
				trust = tr("new — will require verification");
				break;
		}
		item->setText(tr("%1 — %2").arg(recipient.name.toHtmlEscaped()).arg(trust));
		item->setData(Qt::UserRole, recipient.session);
		item->setData(Qt::UserRole + 1, recipient.trustState);
		if (recipient.trustState == static_cast< int >(PQFT::TrustState::Changed)) {
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
	layout->addWidget(m_recipients);

	m_encrypt = new QCheckBox(tr("Encrypt with a password (recipients need it to decrypt)"), this);
	layout->addWidget(m_encrypt);

	m_passwordWidget = new FilePasswordWidget(true, this);
	m_passwordWidget->setEnabled(false);
	layout->addWidget(m_passwordWidget);
	connect(m_encrypt, &QCheckBox::toggled, m_passwordWidget, &QWidget::setEnabled);

	m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(m_buttons);

	connect(m_recipients, &QListWidget::itemChanged, this, [this]() { updateOkState(); });
	connect(m_encrypt, &QCheckBox::toggled, this, [this]() { updateOkState(); });
	for (QLineEdit *edit : m_passwordWidget->findChildren< QLineEdit * >()) {
		connect(edit, &QLineEdit::textChanged, this, [this]() { updateOkState(); });
	}

	updateOkState();
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
