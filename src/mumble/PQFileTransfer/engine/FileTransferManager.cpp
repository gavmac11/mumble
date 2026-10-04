// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferManager.h"

FileTransferManager::FileTransferManager(QObject *parent) : QObject(parent) { }

FileTransferManager::~FileTransferManager() = default;

void FileTransferManager::handleControlMessage(const MumbleProto::FileTransferControl &msg) {
	Q_UNUSED(msg);
	// Session/handshake wiring arrives with the engine phase.
}

void FileTransferManager::handleDataMessage(const MumbleProto::FileData &msg) {
	Q_UNUSED(msg);
	// Chunk reassembly wiring arrives with the engine phase.
}
