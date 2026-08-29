// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "MumbleProtocol.h"
#include "MumbleUDP.pb.h"
#include "VideoFramePacketizer.h"

#include <QByteArray>
#include <QObject>
#include <QTest>

#include <cstddef>
#include <cstdint>
#include <vector>

class TestVideoFramePacketizer : public QObject {
	Q_OBJECT

private slots:
	void rejectsInvalidFrames() const {
		QVERIFY(Mumble::Video::packetizeFrame(1, {}, 2, 1280, 720, false).empty());
		QVERIFY(Mumble::Video::packetizeFrame(1, QByteArray("frame"), 2, 0, 720, false).empty());
		QVERIFY(Mumble::Video::packetizeFrame(1, QByteArray("frame"), 2, 1280, 0, false).empty());
	}

	void preservesFrameMetadataAcrossFragments() const {
		QByteArray encodedData(2000, '\0');
		for (int i = 0; i < encodedData.size(); ++i)
			encodedData[i] = static_cast< char >(i % 127);

		const std::vector< std::vector< unsigned char > > packets =
			Mumble::Video::packetizeFrame(42, encodedData, 99, 1920, 1080, true);
		QCOMPARE(packets.size(), std::size_t(3));

		QByteArray reconstructed;
		for (std::size_t i = 0; i < packets.size(); ++i) {
			const std::vector< unsigned char > &packet = packets[i];
			QVERIFY(!packet.empty());
			QCOMPARE(packet[0], static_cast< unsigned char >(Mumble::Protocol::UDPMessageType::Video));

			MumbleUDP::Video videoMsg;
			QVERIFY(videoMsg.ParseFromArray(packet.data() + 1, static_cast< int >(packet.size() - 1)));
			QCOMPARE(videoMsg.sender_session(), std::uint32_t(42));
			QCOMPARE(videoMsg.codec(), MumbleUDP::Video_Codec_H264);
			QCOMPARE(videoMsg.width(), std::uint32_t(1920));
			QCOMPARE(videoMsg.height(), std::uint32_t(1080));
			QCOMPARE(videoMsg.frame_number(), std::uint64_t(99));
			QCOMPARE(videoMsg.fragment_index(), static_cast< std::uint32_t >(i));
			QCOMPARE(videoMsg.fragment_count(), std::uint32_t(3));
			QCOMPARE(videoMsg.is_keyframe(), i == 0);
			reconstructed.append(videoMsg.video_data().data(), static_cast< qsizetype >(videoMsg.video_data().size()));
		}

		QCOMPARE(reconstructed, encodedData);
	}
};

QTEST_APPLESS_MAIN(TestVideoFramePacketizer)
#include "TestVideoFramePacketizer.moc"
