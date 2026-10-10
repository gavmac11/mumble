// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERTRANSPORT_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERTRANSPORT_H_

#include <QByteArray>
#include <QMetaObject>
#include <QObject>

#include <memory>
#include <optional>

namespace PQFT::Transport {

// The receiver is also the queued call's lifetime context. Keep these calls
// typed: ServerHandler's send methods are ordinary public methods, not slots.
// Copy the buffers before returning to the engine's worker thread.
template< typename Handler >
bool queueControl(Handler *handler, unsigned int targetSession, const QByteArray &payload) {
	if (!handler) {
		return false;
	}
	return QMetaObject::invokeMethod(
		handler, [handler, targetSession, payload]() { handler->sendFileTransferControl({ targetSession }, payload); },
		Qt::QueuedConnection);
}

template< typename Handler >
bool queueChunk(Handler *handler, const QByteArray &transferId, quint64 index, quint64 total, const QByteArray &data) {
	if (!handler) {
		return false;
	}
	return QMetaObject::invokeMethod(
		handler,
		[handler, transferId, index, total, data]() {
			handler->sendFileData(transferId, index, std::optional< quint64 >(total), data);
		},
		Qt::QueuedConnection);
}

// Bind to a connection snapshot, never the mutable global current connection.
// Hold it only while posting; queued calls must not keep a disconnected server alive.
template< typename Handler >
bool queueControl(const std::weak_ptr< Handler > &connection, unsigned int targetSession, const QByteArray &payload) {
	const std::shared_ptr< Handler > handler = connection.lock();
	return queueControl(handler.get(), targetSession, payload);
}

template< typename Handler >
bool queueChunk(const std::weak_ptr< Handler > &connection, const QByteArray &transferId, quint64 index, quint64 total,
				const QByteArray &data) {
	const std::shared_ptr< Handler > handler = connection.lock();
	return queueChunk(handler.get(), transferId, index, total, data);
}

} // namespace PQFT::Transport

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERTRANSPORT_H_
