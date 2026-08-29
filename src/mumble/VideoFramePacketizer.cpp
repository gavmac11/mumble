// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoFramePacketizer.h"

#include "MumbleProtocol.h"
#include "MumbleUDP.pb.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Mumble::Video {

std::vector< std::vector< unsigned char > > packetizeFrame(quint32 senderSession, const QByteArray &encodedData,
														   quint64 frameNumber, quint32 width, quint32 height,
														   bool isKeyFrame) {
	if (encodedData.isEmpty() || width == 0 || height == 0)
		return {};

	// Stay comfortably below common network MTUs after protobuf, encryption, UDP and IP overhead.
	static constexpr int MAX_FRAGMENT_BYTES = 900;
	const int dataSize                      = static_cast< int >(encodedData.size());
	const int fragmentCount                 = (dataSize + MAX_FRAGMENT_BYTES - 1) / MAX_FRAGMENT_BYTES;

	std::vector< std::vector< unsigned char > > packets;
	packets.reserve(static_cast< std::size_t >(fragmentCount));

	for (int i = 0; i < fragmentCount; ++i) {
		const int offset    = i * MAX_FRAGMENT_BYTES;
		const int chunkSize = std::min(MAX_FRAGMENT_BYTES, dataSize - offset);

		MumbleUDP::Video videoMsg;
		videoMsg.set_sender_session(senderSession);
		videoMsg.set_codec(MumbleUDP::Video_Codec_H264);
		videoMsg.set_width(width);
		videoMsg.set_height(height);
		videoMsg.set_frame_number(frameNumber);
		videoMsg.set_fragment_index(static_cast< std::uint32_t >(i));
		videoMsg.set_fragment_count(static_cast< std::uint32_t >(fragmentCount));
		videoMsg.set_video_data(encodedData.constData() + offset, static_cast< std::size_t >(chunkSize));
		videoMsg.set_is_keyframe(isKeyFrame && i == 0);

		const int msgSize = static_cast< int >(videoMsg.ByteSizeLong());
		std::vector< unsigned char > packet(static_cast< std::size_t >(msgSize + 1));
		packet[0] = static_cast< unsigned char >(Mumble::Protocol::UDPMessageType::Video);
		if (!videoMsg.SerializeToArray(packet.data() + 1, msgSize))
			return {};

		packets.push_back(std::move(packet));
	}

	return packets;
}

} // namespace Mumble::Video
