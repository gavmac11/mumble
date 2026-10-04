// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "VideoPacketPacer.h"

#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <memory>

using namespace Mumble::Video;

class TestVideoPacketPacer : public QObject {
	Q_OBJECT

	static VideoPackets frame(std::size_t count, unsigned char value = 1) {
		return VideoPackets(count, VideoPacket(900, value));
	}

private slots:
	void preservesPacketsAndAccountsForWireBytes() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(2, 42), true, 0));
		QVERIFY(queue.enqueue(frame(1, 99), false, 0));
		QCOMPARE(queue.queuedBytes(), std::size_t(3 * 952));
		auto first = queue.takeReady(0);
		QVERIFY(first.has_value());
		QCOMPARE(*first, VideoPacket(900, 42));
		QVERIFY(!queue.takeReady(3'807'999));
		QVERIFY(queue.takeReady(3'808'000));
		auto last = queue.takeReady(7'616'000);
		QVERIFY(last.has_value());
		QCOMPARE(*last, VideoPacket(900, 99));
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QVERIFY(!queue.delayNs(7'616'000));
	}

	void eventLoopStallDoesNotFlushTheWholeFrame() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(20), true, 0));
		QVERIFY(queue.takeReady(0));
		QVERIFY(queue.takeReady(100'000'000));
		QVERIFY(queue.takeReady(100'000'000));
		QVERIFY(!queue.takeReady(100'000'000));
		QCOMPARE(queue.queuedBytes(), std::size_t(17 * 952));
	}

	void idleTimeDoesNotBankBurstCredit() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(1), true, 0));
		QVERIFY(queue.takeReady(0));
		QVERIFY(queue.enqueue(frame(20), false, 1'000'000'000));
		QVERIFY(queue.takeReady(1'000'000'000));
		QVERIFY(!queue.takeReady(1'000'000'000));
	}

	void overloadBoundsMemoryAndWaitsForAnIndependentFrame() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(20), true, 0));
		QVERIFY(queue.enqueue(frame(240), false, 0));
		QVERIFY(!queue.enqueue(frame(30), false, 1));
		QCOMPARE(queue.droppedFrames(), std::uint64_t(2)); // pending tail and rejected input
		QCOMPARE(queue.queuedBytes(), std::size_t(20 * 952));
		QVERIFY(!queue.needsKeyframe()); // Finish the front frame before asking for another IDR.
		for (std::uint64_t i = 0; i < 20; ++i)
			QVERIFY(queue.takeReady(i * 3'808'000));
		QVERIFY(queue.needsKeyframe());
		QVERIFY(!queue.enqueue(frame(1), false, 80'000'000));
		QVERIFY(queue.enqueue(frame(1, 77), true, 80'000'000));
		auto packet = queue.takeReady(80'000'000);
		QVERIFY(packet.has_value());
		QCOMPARE(*packet, VideoPacket(900, 77));
	}

	void invalidFrameDoesNotDestroyAcceptedBacklog() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(2, 19), true, 0));
		QVERIFY(!queue.enqueue(frame(PacketQueue::MaximumFramePackets + 1), true, 0));
		QVERIFY(!queue.enqueue({ VideoPacket(1025) }, true, 0));
		QCOMPARE(queue.queuedBytes(), std::size_t(2 * 952));
		QCOMPARE(queue.droppedFrames(), std::uint64_t(2));
		QCOMPARE(*queue.takeReady(0), VideoPacket(900, 19));
		QCOMPARE(*queue.takeReady(3'808'000), VideoPacket(900, 19));
		QVERIFY(queue.needsKeyframe());
	}

	void largeKeyframeFinishesBeyondTheSoftAgeAndByteBudgets() const {
		PacketQueue queue(2'000'000);
		// About 270 KB of encoded payload: larger than the original queue and a 2 Mbit VBV.
		QVERIFY(queue.enqueue(frame(300, 42), true, 0));
		for (std::uint64_t i = 0; i < 300; ++i) {
			if (i == 1)
				QVERIFY(!queue.enqueue(frame(300), false, i * 3'808'000));
			auto packet = queue.takeReady(i * 3'808'000);
			QVERIFY(packet.has_value());
			QCOMPARE(*packet, VideoPacket(900, 42));
		}
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QCOMPARE(queue.droppedFrames(), std::uint64_t(1));
		QVERIFY(queue.needsKeyframe());
	}

	void expiredPartialFrameIsAbandonedUntilNextKeyframe() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(20), true, 0));
		QVERIFY(queue.takeReady(0));
		const std::uint64_t expired = PacketQueue::SchedulingSlackNs + 20 * 3'808'000;
		QVERIFY(!queue.takeReady(expired));
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QCOMPARE(queue.droppedFrames(), std::uint64_t(1));
		QVERIFY(!queue.enqueue(frame(1), false, expired));
		QVERIFY(queue.enqueue(frame(1), true, expired));
		QVERIFY(queue.takeReady(expired));
	}

	void replacementKeyframeDoesNotBypassTheNextDeadline() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(1), true, 0));
		QVERIFY(queue.takeReady(0));
		QVERIFY(queue.enqueue(frame(10, 17), true, 1));
		QVERIFY(!queue.takeReady(1));
		auto packet = queue.takeReady(3'808'000);
		QVERIFY(packet.has_value());
		QCOMPARE(*packet, VideoPacket(900, 17));
	}

	void exposesDropsAndRequestsRecoveryOnceTheQueueDrains() const {
		PacketPacer pacer(2'000'000, [](const VideoPacket &) {});
		QCOMPARE(qobject_cast< PacketPacer * >(static_cast< QObject * >(&pacer)), &pacer);
		QSignalSpy requests(&pacer, &PacketPacer::keyframeRequested);
		QSignalSpy drops(&pacer, &PacketPacer::framesDropped);
		QTest::ignoreMessage(QtWarningMsg, "Video pacer dropped 1 frames in this share; queued wire bytes: 0");
		QVERIFY(!pacer.enqueue(frame(1), false));
		QCOMPARE(requests.size(), 1);
		QVERIFY(!pacer.enqueue(frame(1), false));
		QCOMPARE(requests.size(), 1);
		QVERIFY(pacer.enqueue(frame(1), true));
		QVERIFY(!pacer.enqueue({ VideoPacket(1025) }, false));
		QCOMPARE(requests.size(), 1);
		QTRY_COMPARE(requests.size(), 2);
		QCOMPARE(pacer.droppedFrames(), std::uint64_t(3));
		QCOMPARE(drops.size(), 3);
		QCOMPARE(drops.back().front().toULongLong(), qulonglong(3));
	}

	void timerIsAsynchronousAndDestructionCancelsPendingPackets() const {
		int sent = 0;
		QEventLoop loop;
		QTimer timeout;
		timeout.setSingleShot(true);
		connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		auto pacer = std::make_unique< PacketPacer >(100'000, [&sent, &loop](const VideoPacket &) {
			++sent;
			loop.quit();
		});
		QVERIFY(pacer->enqueue(frame(3), true));
		QCOMPARE(sent, 0);
		// A delayed callback may dispatch the one permitted catch-up packet too.
		timeout.start(1000);
		loop.exec();
		QVERIFY(sent >= 1 && sent <= 2);
		const int beforeDestruction = sent;
		pacer.reset();
		QTest::qWait(120);
		QCOMPARE(sent, beforeDestruction);
	}
};

QTEST_GUILESS_MAIN(TestVideoPacketPacer)
#include "TestVideoPacketPacer.moc"
