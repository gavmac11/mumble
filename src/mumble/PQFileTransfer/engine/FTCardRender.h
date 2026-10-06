// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Chat file cards: the log embeds a data URL of type
// application/mumble-file whose payload is the 16-byte transfer id; the
// document resolves it to a rendered card showing name, size, progress,
// state and — when password mode is on — an encrypted badge.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTCARDRENDER_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTCARDRENDER_H_

#include "PQFileTransfer/engine/FileTransferEngine.h"

#include <QByteArray>
#include <QImage>
#include <QUrl>

namespace PQFT {

/// HTML for embedding a transfer's card into the log.
QString fileCardToHtml(const QByteArray &transferId);

/// Extract the transfer id from a mumble-file data URL.
QByteArray fileCardTransferId(const QUrl &url, bool &ok);

/// Render the card. `width` is the logical width in pixels; the image is
/// produced at `dpr` device pixels per logical pixel for crisp text.
QImage renderFileCard(const FTTransferInfo &info, int width, qreal dpr);

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTCARDRENDER_H_
