// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOFRAMEPACKETIZER_H_
#define MUMBLE_MUMBLE_VIDEOFRAMEPACKETIZER_H_

#include <QtCore/QByteArray>
#include <QtCore/QtTypes>

#include <vector>

namespace Mumble::Video {

/// Splits an encoded H.264 frame into UDP-safe MumbleUDP::Video packets.
std::vector< std::vector< unsigned char > > packetizeFrame(quint32 senderSession, const QByteArray &encodedData,
														   quint64 frameNumber, quint32 width, quint32 height,
														   bool isKeyFrame);

} // namespace Mumble::Video

#endif // MUMBLE_MUMBLE_VIDEOFRAMEPACKETIZER_H_
