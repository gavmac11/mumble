// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoQualityProfile.h"

#include <QObject>
#include <QSize>
#include <QTest>

class TestVideoQualityProfile : public QObject {
	Q_OBJECT

private slots:
	void definesSourceAppropriateDefaults() const {
		const Mumble::VideoQuality::Profile &screen = Mumble::VideoQuality::screenShareProfile();
		QCOMPARE(screen.maximumFrameSize, QSize(1920, 1080));
		QCOMPARE(screen.framesPerSecond, 15);
		QCOMPARE(screen.bitRate, 2'000'000);
		QCOMPARE(screen.keyFrameInterval, 5);

		const Mumble::VideoQuality::Profile &webcam = Mumble::VideoQuality::webcamProfile();
		QCOMPARE(webcam.maximumFrameSize, QSize(1280, 720));
		QCOMPARE(webcam.framesPerSecond, 30);
		QCOMPARE(webcam.bitRate, 1'500'000);
		QCOMPARE(webcam.keyFrameInterval, 10);
	}

	void preservesSupportedEvenSizes() const {
		const QSize source(1280, 720);
		QCOMPARE(Mumble::VideoQuality::constrainedFrameSize(source, Mumble::VideoQuality::screenShareProfile()),
				 source);
	}

	void roundsOddDimensionsDown() const {
		QCOMPARE(
			Mumble::VideoQuality::constrainedFrameSize(QSize(1279, 719), Mumble::VideoQuality::screenShareProfile()),
			QSize(1278, 718));
	}

	void constrainsScreenContentTo1080p() const {
		const Mumble::VideoQuality::Profile &profile = Mumble::VideoQuality::screenShareProfile();
		QCOMPARE(Mumble::VideoQuality::constrainedFrameSize(QSize(3840, 2160), profile), QSize(1920, 1080));
		QCOMPARE(Mumble::VideoQuality::constrainedFrameSize(QSize(3440, 1440), profile), QSize(1920, 802));
		QCOMPARE(Mumble::VideoQuality::constrainedFrameSize(QSize(2160, 3840), profile), QSize(606, 1080));
	}

	void constrainsWebcamContentTo720p() const {
		const Mumble::VideoQuality::Profile &profile = Mumble::VideoQuality::webcamProfile();
		QCOMPARE(Mumble::VideoQuality::constrainedFrameSize(QSize(1920, 1080), profile), QSize(1280, 720));
	}

	void rejectsInvalidAndTinyFrames() const {
		const Mumble::VideoQuality::Profile &profile = Mumble::VideoQuality::screenShareProfile();
		QVERIFY(!Mumble::VideoQuality::constrainedFrameSize(QSize(), profile).isValid());
		QVERIFY(!Mumble::VideoQuality::constrainedFrameSize(QSize(1, 2), profile).isValid());
	}
};

QTEST_APPLESS_MAIN(TestVideoQualityProfile)
#include "TestVideoQualityProfile.moc"
