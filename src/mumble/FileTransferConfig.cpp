// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "FileTransferConfig.h"

#include "Global.h"
#include "MainWindow.h"
#include "PQFileTransfer/engine/FileTransferManager.h"

#include <QFileDialog>

static ConfigWidget *FileTransferConfigNew(Settings &st) {
	return new FileTransferConfig(st);
}

static ConfigRegistrar registrarFT(6100, FileTransferConfigNew);

const QString FileTransferConfig::name = QLatin1String("FileTransferConfig");

FileTransferConfig::FileTransferConfig(Settings &st) : ConfigWidget(st) {
	setupUi(this);
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
	s.qsFTDownloadDir   = qleFTDownloadDir->text();
	s.iFTChunkKB        = qsbFTChunkSize->value();
	s.iFTSendPaceKiB    = qsbFTSendPace->value();
	s.iFTMaxReceiveMiB  = qsbFTMaxReceive->value();
}

void FileTransferConfig::load(const Settings &r) {
	qcbFTEnabled->setChecked(r.bFTEnabled);
	qcbFTAutoAccept->setChecked(r.bFTAutoAcceptPinned);
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
