// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "VideoPacketPacer.h"

#include <QEventLoop>
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
		QVERIFY(queue.enqueue(frame(60), true, 0));
		QVERIFY(!queue.enqueue(frame(10), false, 1));
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QVERIFY(!queue.enqueue(frame(1), false, 2));
		QVERIFY(queue.enqueue(frame(1, 77), true, 3));
		auto packet = queue.takeReady(3);
		QVERIFY(packet.has_value());
		QCOMPARE(*packet, VideoPacket(900, 77));
	}

	void oversizedFrameCannotOccupyAnUnboundedQueue() const {
		PacketQueue queue(2'000'000);
		QVERIFY(!queue.enqueue(frame(1000), true, 0));
		QVERIFY(!queue.enqueue({ VideoPacket(1025) }, true, 0));
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QVERIFY(queue.enqueue(frame(1), true, 0));
	}

	void expiredPartialFrameIsAbandonedUntilNextKeyframe() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(20), true, 0));
		QVERIFY(queue.takeReady(0));
		QVERIFY(!queue.takeReady(PacketQueue::MaximumAgeNs));
		QCOMPARE(queue.queuedBytes(), std::size_t(0));
		QVERIFY(!queue.enqueue(frame(1), false, PacketQueue::MaximumAgeNs));
		QVERIFY(queue.enqueue(frame(1), true, PacketQueue::MaximumAgeNs));
		QVERIFY(queue.takeReady(PacketQueue::MaximumAgeNs));
	}

	void replacementKeyframeDoesNotBypassTheNextDeadline() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(60), true, 0));
		QVERIFY(queue.takeReady(0));
		QVERIFY(queue.enqueue(frame(10, 17), true, 1));
		QVERIFY(!queue.takeReady(1));
		auto packet = queue.takeReady(3'808'000);
		QVERIFY(packet.has_value());
		QCOMPARE(*packet, VideoPacket(900, 17));
	}

	void clearDiscardsPreviousSessionAndDecoderDependencies() const {
		PacketQueue queue(2'000'000);
		QVERIFY(queue.enqueue(frame(10), true, 0));
		queue.clear();
		QVERIFY(!queue.takeReady(1));
		QVERIFY(!queue.enqueue(frame(1), false, 2));
		QVERIFY(queue.enqueue(frame(1, 9), true, 3));
		QCOMPARE(*queue.takeReady(3), VideoPacket(900, 9));
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
		// Return after the first callback, even if a busy runner delayed it enough
		// to make the permitted catch-up packet immediately ready.
		timeout.start(1000);
		loop.exec();
		QCOMPARE(sent, 1);
		pacer.reset();
		QTest::qWait(120);
		QCOMPARE(sent, 1);
	}
};

QTEST_GUILESS_MAIN(TestVideoPacketPacer)
#include "TestVideoPacketPacer.moc"
