// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoQualityProfile.h"
#include "VideoFramePacketizer.h"
#include "VideoPacketPacer.h"

#include <QtCore/Qt>
#include <algorithm>

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

namespace {
	constexpr std::uint64_t DefaultServerVideoBitRate = 2'500'000;
	constexpr std::uint64_t MaximumWireBitRate        = DefaultServerVideoBitRate * 24 / 25;
	constexpr std::uint64_t FullPacketWireBytes =
		Video::PacketQueue::MaximumPacketBytes + Video::PacketQueue::WireOverhead;
	constexpr std::uint64_t PayloadBytes = Video::MaximumFragmentBytes;
} // namespace

int encoderBitRate(const Profile &profile) {
	return std::clamp(profile.bitRate, 1, static_cast< int >(MaximumWireBitRate * PayloadBytes / FullPacketWireBytes));
}

std::uint64_t wireBitRate(const Profile &profile) {
	const auto wireRate =
		(static_cast< std::uint64_t >(encoderBitRate(profile)) * FullPacketWireBytes + PayloadBytes - 1) / PayloadBytes;
	// Allow 10% scheduling/encoder burst headroom where the default server ceiling permits it.
	return std::min(MaximumWireBitRate, (wireRate * 11 + 9) / 10);
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
