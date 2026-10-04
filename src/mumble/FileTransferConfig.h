// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_FILETRANSFERCONFIG_H_
#define MUMBLE_MUMBLE_FILETRANSFERCONFIG_H_

#include "ConfigWidget.h"
#include "ui_FileTransferConfig.h"

/// Settings page for the chat file-transfer feature (chunking, pacing,
/// receive limits, download directory).
class FileTransferConfig : public ConfigWidget, public Ui::FileTransferConfig {
private:
	Q_OBJECT
	Q_DISABLE_COPY(FileTransferConfig)

public:
	/// The unique name of this ConfigWidget
	static const QString name;

	FileTransferConfig(Settings &st);

	QString title() const Q_DECL_OVERRIDE;
	const QString &getName() const Q_DECL_OVERRIDE;
	QIcon icon() const Q_DECL_OVERRIDE;
	void save() const Q_DECL_OVERRIDE;
	void load(const Settings &r) Q_DECL_OVERRIDE;
	/// Pushes the (possibly changed) limits into the transfer engine.
	void accept() const Q_DECL_OVERRIDE;

protected slots:
	void on_qpbFTBrowse_clicked();
};

#endif // MUMBLE_MUMBLE_FILETRANSFERCONFIG_H_
