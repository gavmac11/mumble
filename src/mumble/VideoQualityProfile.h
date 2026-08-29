// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOQUALITYPROFILE_H_
#define MUMBLE_MUMBLE_VIDEOQUALITYPROFILE_H_

#include <QtCore/QSize>

namespace Mumble::VideoQuality {

struct Profile {
	QSize maximumFrameSize;
	int framesPerSecond  = 0;
	int bitRate          = 0;
	int keyFrameInterval = 0;
};

/// Screen content favours readable detail over motion while remaining below the server's
/// default 2.5 Mbit/s per-user video ceiling after packet overhead.
const Profile &screenShareProfile();

/// Webcam content favours motion and matches the broadly supported UVC 720p30 mode.
const Profile &webcamProfile();

/// Returns an even-sized frame that fits the profile without enlarging the source.
/// An invalid size is returned when the source cannot produce a YUV420-compatible frame.
QSize constrainedFrameSize(const QSize &sourceSize, const Profile &profile);

} // namespace Mumble::VideoQuality

#endif // MUMBLE_MUMBLE_VIDEOQUALITYPROFILE_H_
