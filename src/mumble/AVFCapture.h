// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_AVFCAPTURE_H_
#define MUMBLE_MUMBLE_AVFCAPTURE_H_

#if defined(Q_OS_MAC) && defined(USE_SCREEN_SHARING)

#	include <QtCore/QString>
#	include <QtGui/QImage>
#	include <functional>

/// Starts capturing the AVCaptureDevice identified by uniqueID.
/// Camera permission is requested when needed. All callbacks run on the Qt/Cocoa main thread.
void avf_startCamera(const QString &uniqueID, std::function< void() > onStarted,
					 std::function< void(QString) > onError, std::function< void(QImage) > onFrame);

/// Stops the active camera capture, including one that is still waiting for permission.
void avf_stopCamera();

#endif // Q_OS_MAC && USE_SCREEN_SHARING
#endif // MUMBLE_MUMBLE_AVFCAPTURE_H_
