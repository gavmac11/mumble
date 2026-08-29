// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoQualityProfile.h"

#include <QtCore/Qt>

namespace Mumble::VideoQuality {

const Profile &screenShareProfile() {
	// 1080p15 at 2 Mbit/s is a pragmatic default for text-heavy screen content. The bitrate
	// leaves headroom under the server's 2.5 Mbit/s ceiling for UDP and protobuf overhead.
	static const Profile profile{ QSize(1920, 1080), 15, 2'000'000, 5 };
	return profile;
}

const Profile &webcamProfile() {
	static const Profile profile{ QSize(1280, 720), 30, 1'500'000, 10 };
	return profile;
}

QSize constrainedFrameSize(const QSize &sourceSize, const Profile &profile) {
	if (!sourceSize.isValid() || !profile.maximumFrameSize.isValid())
		return {};

	QSize result = sourceSize;
	if (result.width() > profile.maximumFrameSize.width() || result.height() > profile.maximumFrameSize.height()) {
		result.scale(profile.maximumFrameSize, Qt::KeepAspectRatio);
	}

	// libx264's YUV420P input requires even dimensions. Rounding down by one pixel avoids
	// exceeding either the source size or the selected profile.
	result.setWidth(result.width() & ~1);
	result.setHeight(result.height() & ~1);
	if (result.width() < 2 || result.height() < 2)
		return {};

	return result;
}

} // namespace Mumble::VideoQuality
