// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "VideoPacketPacer.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace Mumble::Video {

PacketQueue::PacketQueue(std::uint64_t bitsPerSecond) : m_bitsPerSecond(bitsPerSecond) {
	if (bitsPerSecond == 0 || bitsPerSecond > 100'000'000)
		throw std::invalid_argument("Invalid video pacing rate");
	// At most 250 ms of wire bytes can wait in memory, including packet overhead.
	m_maximumBytes = static_cast< std::size_t >(bitsPerSecond / 32);
}

bool PacketQueue::stale(std::uint64_t nowNs) const {
	return !m_frames.empty() && nowNs - m_frames.front().createdNs >= MaximumAgeNs;
}

void PacketQueue::discardBacklog() {
	m_frames.clear();
	m_queuedBytes   = 0;
	m_needsKeyframe = true;
	// Preserve the next send deadline so a replacement keyframe cannot bypass pacing.
}

void PacketQueue::clear() {
	discardBacklog();
	m_dueNs = 0;
}

bool PacketQueue::enqueue(VideoPackets packets, bool keyframe, std::uint64_t nowNs) {
	if (packets.empty())
		return false;
	std::size_t bytes = 0;
	for (const auto &packet : packets) {
		if (packet.empty() || packet.size() > 1024 || bytes + packet.size() + WireOverhead > m_maximumBytes) {
			discardBacklog();
			return false;
		}
		bytes += packet.size() + WireOverhead;
	}
	if (stale(nowNs) || m_queuedBytes + bytes > m_maximumBytes)
		discardBacklog();
	if (m_needsKeyframe && !keyframe)
		return false;
	if (m_frames.empty())
		m_dueNs = std::max(m_dueNs, nowNs);
	m_frames.push_back({ std::move(packets), 0, nowNs });
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
	const std::uint64_t interval = (bytes * 8'000'000'000ULL + m_bitsPerSecond - 1) / m_bitsPerSecond;
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
	if (!m_timer.isActive())
		schedule();
	return accepted;
}

void PacketPacer::schedule() {
	if (const auto delay = m_queue.delayNs(nowNs()))
		m_timer.start(static_cast< int >((*delay + 999'999) / 1'000'000));
}

void PacketPacer::sendNext() {
	if (auto packet = m_queue.takeReady(nowNs()))
		m_sink(*packet);
	schedule();
}

} // namespace Mumble::Video
