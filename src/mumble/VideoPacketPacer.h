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
/// Overload preserves the front frame, then requests an independent frame after draining.
class PacketQueue {
public:
	static constexpr std::uint64_t SchedulingSlackNs = 250'000'000;
	static constexpr std::size_t WireOverhead        = 52;
	static constexpr std::size_t MaximumPacketBytes  = 1024;
	// Match the receiver's fragment-count bound; a single IDR may exceed the soft queue budget.
	static constexpr std::size_t MaximumFramePackets = 4096;

	explicit PacketQueue(std::uint64_t bitsPerSecond);
	bool enqueue(VideoPackets packets, bool keyframe, std::uint64_t nowNs);
	std::optional< VideoPacket > takeReady(std::uint64_t nowNs);
	std::optional< std::uint64_t > delayNs(std::uint64_t nowNs) const;
	std::size_t queuedBytes() const;
	std::uint64_t droppedFrames() const;
	bool needsKeyframe() const;

private:
	struct Frame {
		VideoPackets packets;
		std::size_t next           = 0;
		std::size_t remainingBytes = 0;
		std::uint64_t expiresNs    = 0;
	};
	std::deque< Frame > m_frames;
	std::uint64_t m_bitsPerSecond;
	std::size_t m_maximumBytes;
	std::size_t m_queuedBytes     = 0;
	std::uint64_t m_dueNs         = 0;
	bool m_needsKeyframe          = true;
	std::uint64_t m_droppedFrames = 0;
	void discardBacklog();
	void retainFrontFrame();
	bool rejectFrame();
	std::uint64_t transmissionNs(std::size_t bytes) const;
	bool stale(std::uint64_t nowNs) const;
};

/// GUI-thread adapter. Each callback drains due packets with bounded catch-up; voice bypasses it.
class PacketPacer : public QObject {
	Q_OBJECT
public:
	using Sink = std::function< void(const VideoPacket &) >;
	PacketPacer(std::uint64_t bitsPerSecond, Sink sink, QObject *parent = nullptr);
	bool enqueue(VideoPackets packets, bool keyframe);
	std::uint64_t droppedFrames() const;

signals:
	void keyframeRequested();
	void framesDropped(quint64 total);

private:
	PacketQueue m_queue;
	Sink m_sink;
	QTimer m_timer;
	std::uint64_t m_reportedDrops = 0;
	std::uint64_t m_nextWarningNs = 0;
	bool m_recoveryRequested      = false;
	static std::uint64_t nowNs();
	void schedule();
	void sendNext();
	void reportState();
};

} // namespace Mumble::Video

#endif
