// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef MUMBLE_MUMBLE_VIDEOPACKETPACER_H_
#define MUMBLE_MUMBLE_VIDEOPACKETPACER_H_

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace Mumble::Video {

using VideoPacket  = std::vector< unsigned char >;
using VideoPackets = std::vector< VideoPacket >;

/// Monotonic-clock scheduler. Wire accounting includes IPv6, UDP and encryption overhead.
/// A stale/overloaded stream discards its backlog and waits for a new independent keyframe.
class PacketQueue {
public:
	static constexpr std::uint64_t MaximumAgeNs = 250'000'000;
	static constexpr std::size_t WireOverhead   = 52;

	explicit PacketQueue(std::uint64_t bitsPerSecond);
	bool enqueue(VideoPackets packets, bool keyframe, std::uint64_t nowNs);
	std::optional< VideoPacket > takeReady(std::uint64_t nowNs);
	std::optional< std::uint64_t > delayNs(std::uint64_t nowNs) const;
	void clear();
	std::size_t queuedBytes() const;

private:
	struct Frame {
		VideoPackets packets;
		std::size_t next        = 0;
		std::uint64_t createdNs = 0;
	};
	std::deque< Frame > m_frames;
	std::uint64_t m_bitsPerSecond;
	std::size_t m_maximumBytes;
	std::size_t m_queuedBytes = 0;
	std::uint64_t m_dueNs     = 0;
	bool m_needsKeyframe      = true;
	void discardBacklog();
	bool stale(std::uint64_t nowNs) const;
};

/// GUI-thread adapter. Each timer callback sends at most one packet; voice bypasses this queue.
class PacketPacer : public QObject {
public:
	using Sink = std::function< void(const VideoPacket &) >;
	PacketPacer(std::uint64_t bitsPerSecond, Sink sink, QObject *parent = nullptr);
	bool enqueue(VideoPackets packets, bool keyframe);

private:
	PacketQueue m_queue;
	Sink m_sink;
	QTimer m_timer;
	static std::uint64_t nowNs();
	void schedule();
	void sendNext();
};

} // namespace Mumble::Video

#endif
