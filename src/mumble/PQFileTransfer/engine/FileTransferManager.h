// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// FileTransferManager is the front door of the chat file-transfer feature.
// The ServerHandler thread calls the two handle*Message() entry points
// directly (cheap parse + hand-off to the worker); everything else —
// sessions, crypto, file IO — lives on the manager's worker thread and is
// added by the session/engine phases. See PROTOCOL.md.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_

#include <Mumble.pb.h>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <optional>

class FileTransferManager : public QObject {
	Q_OBJECT
public:
	explicit FileTransferManager(QObject *parent = nullptr);
	~FileTransferManager() override;

	/// Called on the ServerHandler thread for every FileTransferControl the
	/// server routes to us. Performs the cheap front-half work only.
	void handleControlMessage(const MumbleProto::FileTransferControl &msg);

	/// Called on the ServerHandler thread for every FileData chunk addressed
	/// to our channel. Performs the cheap front-half work only.
	void handleDataMessage(const MumbleProto::FileData &msg);
};

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_
