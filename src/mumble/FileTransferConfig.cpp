// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "FileTransferConfig.h"

#include "MainWindow.h"
#include "PQFileTransfer/engine/FileTransferManager.h"
#include "PQFileTransfer/widgets/FileTransferDialogs.h"
#include "Global.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>

static ConfigWidget *FileTransferConfigNew(Settings &st) {
	return new FileTransferConfig(st);
}

static ConfigRegistrar registrarFT(6100, FileTransferConfigNew);

const QString FileTransferConfig::name = QLatin1String("FileTransferConfig");

FileTransferConfig::FileTransferConfig(Settings &st) : ConfigWidget(st) {
	setupUi(this);
	updateIdentityStatus();
}

QString FileTransferConfig::title() const {
	return windowTitle();
}

const QString &FileTransferConfig::getName() const {
	return FileTransferConfig::name;
}

QIcon FileTransferConfig::icon() const {
	return QIcon(QLatin1String("skin:config_msgs.png"));
}

void FileTransferConfig::save() const {
	s.bFTEnabled        = qcbFTEnabled->isChecked();
	s.bFTAutoAcceptPinned = qcbFTAutoAccept->isChecked();
	s.bFTRequireVerification = qcbFTRequireVerification->isChecked();
	s.qsFTDownloadDir   = qleFTDownloadDir->text();
	s.iFTChunkKB        = qsbFTChunkSize->value();
	s.iFTSendPaceKiB    = qsbFTSendPace->value();
	s.iFTMaxReceiveMiB  = qsbFTMaxReceive->value();
}

void FileTransferConfig::load(const Settings &r) {
	qcbFTEnabled->setChecked(r.bFTEnabled);
	qcbFTAutoAccept->setChecked(r.bFTAutoAcceptPinned);
	qcbFTRequireVerification->setChecked(r.bFTRequireVerification);
	updateIdentityStatus();
	qleFTDownloadDir->setText(r.qsFTDownloadDir);
	qsbFTChunkSize->setValue(r.iFTChunkKB);
	qsbFTSendPace->setValue(r.iFTSendPaceKiB);
	qsbFTMaxReceive->setValue(r.iFTMaxReceiveMiB);
}

void FileTransferConfig::accept() const {
	ConfigWidget::accept();
	// Apply the new limits to a running engine immediately
	if (Global::get().fileTransferManager) {
		Global::get().fileTransferManager->applyEngineConfig();
	}
}

void FileTransferConfig::on_qpbFTBrowse_clicked() {
	const QString dir = QFileDialog::getExistingDirectory(
		this, tr("Choose the download directory"),
		qleFTDownloadDir->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
										   : qleFTDownloadDir->text());
	if (!dir.isEmpty()) {
		qleFTDownloadDir->setText(dir);
	}
}

void FileTransferConfig::updateIdentityStatus() {
	FileTransferManager *manager   = Global::get().fileTransferManager;
	FileTransferIdentity *identity = manager ? manager->identity() : nullptr;
	if (!identity) {
		qlFTIdentityStatus->setText(tr("File sharing is not available."));
		qpbFTPassphrase->setVisible(false);
		return;
	}
	qpbFTPassphrase->setVisible(true);
	if (identity->protection() == FileTransferIdentity::Protection::Passphrase) {
		qlFTIdentityStatus->setText(tr("Your file-sharing key is protected by a passphrase, which you enter when "
									   "you connect."));
		qpbFTPassphrase->setText(tr("&Remove passphrase"));
	} else {
		qlFTIdentityStatus->setText(tr("Your file-sharing key unlocks automatically on this computer."));
		qpbFTPassphrase->setText(tr("Set &passphrase…"));
	}
}

void FileTransferConfig::on_qpbFTPassphrase_clicked() {
	FileTransferManager *manager   = Global::get().fileTransferManager;
	FileTransferIdentity *identity = manager ? manager->identity() : nullptr;
	if (!identity)
		return;

	// Takes effect right away, independent of OK/Cancel: the key lives in the
	// database, not in the settings.
	if (identity->protection() == FileTransferIdentity::Protection::Passphrase) {
		while (!identity->isUnlocked()) {
			bool ok = false;
			const QString passphrase =
				QInputDialog::getText(this, tr("Remove passphrase"), tr("Enter your current passphrase:"),
									  QLineEdit::Password, QString(), &ok);
			if (!ok)
				return;
			if (!identity->unlock(passphrase))
				QMessageBox::information(this, tr("Remove passphrase"), tr("Wrong passphrase."));
		}
		if (!identity->removePassphrase()) {
			QMessageBox::warning(this, tr("Remove passphrase"), tr("Could not remove the passphrase. Try again."));
			return;
		}
		s.bFTIdentityPassphraseChosen = Global::get().s.bFTIdentityPassphraseChosen = false;
	} else {
		const bool unlocked =
			identity->isUnlocked()
			|| (identity->hasIdentity() ? identity->unlockWithDevice() : identity->createDeviceIdentity());
		if (!unlocked) {
			QMessageBox::warning(this, tr("Set passphrase"),
								 tr("Your file-sharing key cannot be unlocked on this computer. Send a file to set up "
									"a new one first."));
			return;
		}
		FileIdentityPassphraseDialog dialog(this);
		if (dialog.exec() != QDialog::Accepted)
			return;
		if (!identity->changePassphrase(dialog.passphrase())) {
			QMessageBox::warning(this, tr("Set passphrase"), tr("Could not set the passphrase. Try again."));
			return;
		}
		s.bFTIdentityPassphraseChosen = Global::get().s.bFTIdentityPassphraseChosen = true;
	}
	if (manager->hasUsableIdentity())
		manager->pushIdentityToEngine();
	updateIdentityStatus();
}
