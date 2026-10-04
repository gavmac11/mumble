// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "VideoPacketPacer.h"

#include <QtCore/QDebug>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace Mumble::Video {

PacketQueue::PacketQueue(std::uint64_t bitsPerSecond) : m_bitsPerSecond(bitsPerSecond) {
	if (bitsPerSecond == 0 || bitsPerSecond > 100'000'000)
		throw std::invalid_argument("Invalid video pacing rate");
	// Match the encoder's one-second VBV reservoir for frames waiting behind the
	// front frame. Large front frames have their own bounded serialization allowance.
	m_maximumBytes = static_cast< std::size_t >(bitsPerSecond / 8);
}

bool PacketQueue::stale(std::uint64_t nowNs) const {
	return !m_frames.empty() && nowNs >= m_frames.front().expiresNs;
}

void PacketQueue::discardBacklog() {
	m_droppedFrames += m_frames.size();
	m_frames.clear();
	m_queuedBytes   = 0;
	m_needsKeyframe = true;
	// Preserve the next send deadline so a replacement keyframe cannot bypass pacing.
}

void PacketQueue::retainFrontFrame() {
	while (m_frames.size() > 1) {
		m_queuedBytes -= m_frames.back().remainingBytes;
		m_frames.pop_back();
		++m_droppedFrames;
	}
	m_needsKeyframe = true;
}

bool PacketQueue::rejectFrame() {
	++m_droppedFrames;
	// Even a frame rejected before transmission may be an encoder reference for
	// later P-frames. Without non-reference metadata, recovery needs a new IDR.
	m_needsKeyframe = true;
	return false;
}

std::uint64_t PacketQueue::transmissionNs(std::size_t bytes) const {
	return (bytes * 8'000'000'000ULL + m_bitsPerSecond - 1) / m_bitsPerSecond;
}

bool PacketQueue::enqueue(VideoPackets packets, bool keyframe, std::uint64_t nowNs) {
	if (packets.empty() || packets.size() > MaximumFramePackets)
		return rejectFrame();
	std::size_t bytes = 0;
	for (const auto &packet : packets) {
		// Invalid/oversized input must not destroy already accepted, decodable work.
		if (packet.empty() || packet.size() > MaximumPacketBytes)
			return rejectFrame();
		bytes += packet.size() + WireOverhead;
	}
	if (stale(nowNs))
		discardBacklog();
	if (m_needsKeyframe && (!keyframe || !m_frames.empty()))
		return rejectFrame();
	if (!m_frames.empty() && m_queuedBytes - m_frames.front().remainingBytes + bytes > m_maximumBytes) {
		// Finish the frame already in flight before asking the encoder to restart.
		retainFrontFrame();
		return rejectFrame();
	}
	if (m_frames.empty())
		m_dueNs = std::max(m_dueNs, nowNs);
	// A legal large IDR needs its serialization time in addition to scheduling slack.
	const auto expiresNs = nowNs + SchedulingSlackNs + transmissionNs(m_queuedBytes + bytes);
	m_frames.push_back({ std::move(packets), 0, bytes, expiresNs });
	m_queuedBytes += bytes;
	m_needsKeyframe = false;
	return true;
}

std::optional< VideoPacket > PacketQueue::takeReady(std::uint64_t nowNs) {
	if (stale(nowNs))
		discardBacklog();
	if (m_frames.empty() || nowNs < m_dueNs)
		return std::nullopt;
	auto &frame        = m_frames.front();
	VideoPacket packet = std::move(frame.packets[frame.next++]);
	const auto bytes   = packet.size() + WireOverhead;
	m_queuedBytes -= bytes;
	frame.remainingBytes -= bytes;
	const std::uint64_t interval = transmissionNs(bytes);
	// Carry fractional-millisecond deadlines across Qt timer wakeups, but permit
	// no more than one packet of catch-up after the event loop has been delayed.
	m_dueNs = std::max(m_dueNs + interval, nowNs);
	if (frame.next == frame.packets.size())
		m_frames.pop_front();
	return packet;
}

std::optional< std::uint64_t > PacketQueue::delayNs(std::uint64_t nowNs) const {
	if (m_frames.empty())
		return std::nullopt;
	return m_dueNs > nowNs ? m_dueNs - nowNs : 0;
}

std::size_t PacketQueue::queuedBytes() const {
	return m_queuedBytes;
}

std::uint64_t PacketQueue::droppedFrames() const {
	return m_droppedFrames;
}

bool PacketQueue::needsKeyframe() const {
	return m_needsKeyframe && m_frames.empty();
}

PacketPacer::PacketPacer(std::uint64_t bitsPerSecond, Sink sink, QObject *parent)
	: QObject(parent), m_queue(bitsPerSecond), m_sink(std::move(sink)), m_timer(this) {
	m_timer.setSingleShot(true);
	m_timer.setTimerType(Qt::PreciseTimer);
	connect(&m_timer, &QTimer::timeout, this, [this]() { sendNext(); });
}

std::uint64_t PacketPacer::nowNs() {
	return static_cast< std::uint64_t >(
		std::chrono::duration_cast< std::chrono::nanoseconds >(std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

bool PacketPacer::enqueue(VideoPackets packets, bool keyframe) {
	const bool accepted = m_queue.enqueue(std::move(packets), keyframe, nowNs());
	if (accepted && keyframe)
		m_recoveryRequested = false;
	reportState();
	if (!m_timer.isActive())
		schedule();
	return accepted;
}

void PacketPacer::schedule() {
	if (const auto delay = m_queue.delayNs(nowNs()))
		m_timer.start(static_cast< int >((*delay + 999'999) / 1'000'000));
}

void PacketPacer::sendNext() {
	// Use one timestamp so callback/sink work cannot turn catch-up into an unbounded loop.
	const auto now = nowNs();
	while (auto packet = m_queue.takeReady(now))
		m_sink(*packet);
	reportState();
	schedule();
}

std::uint64_t PacketPacer::droppedFrames() const {
	return m_queue.droppedFrames();
}

void PacketPacer::reportState() {
	const auto dropped = m_queue.droppedFrames();
	if (dropped != m_reportedDrops) {
		m_reportedDrops = dropped;
		emit framesDropped(static_cast< quint64 >(dropped));
		const auto now = nowNs();
		if (now >= m_nextWarningNs) {
			qWarning("Video pacer dropped %llu frames in this share; queued wire bytes: %zu",
					 static_cast< unsigned long long >(dropped), m_queue.queuedBytes());
			m_nextWarningNs = now + 5'000'000'000ULL;
		}
	}
	if (m_queue.needsKeyframe() && !m_recoveryRequested) {
		m_recoveryRequested = true;
		emit keyframeRequested();
	}
}

} // namespace Mumble::Video
