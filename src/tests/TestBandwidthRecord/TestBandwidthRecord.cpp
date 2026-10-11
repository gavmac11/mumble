// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "BandwidthRecord.h"

#include <QTest>
#include <algorithm>
#include <chrono>
#include <limits>

class ElapsedTimer : public Timer {
public:
	explicit ElapsedTimer(std::chrono::microseconds age) { m_start = std::chrono::steady_clock::now() - age; }
};

class TestBandwidthRecord : public QObject {
	Q_OBJECT

	static void ageNextSlot(BandwidthRecord &record, std::chrono::microseconds age = std::chrono::seconds(1000)) {
		record.a_qtWhen[record.iRecNum] = ElapsedTimer(age);
	}

private slots:
	void fileChunksRetireTheirEntireByteCharge() const {
		BandwidthRecord record;
		constexpr int chunk = 1024 * 1024 + 34;
		for (int i = 0; i < N_BANDWIDTH_SLOTS * 3; ++i) {
			ageNextSlot(record);
			QVERIFY(record.addFrame(chunk, std::numeric_limits< int >::max()));
			const qint64 expected = static_cast< qint64 >(std::min(i + 1, N_BANDWIDTH_SLOTS)) * chunk;
			QCOMPARE(static_cast< qint64 >(record.iSum), expected);
		}
	}

	void rejectsExcessAtTheWindowsMultiplicationBoundary() const {
		BandwidthRecord record;
		ageNextSlot(record, std::chrono::seconds(1));
		// 3000 * 1,000,000 exceeds a 32-bit long, used by Windows.
		QVERIFY(!record.addFrame(3000, 1));
		QCOMPARE(record.iSum, 0);
	}

	void longIdleSlotStillEnforcesTheConfiguredRate() const {
		BandwidthRecord record;
		// Forty minutes in microseconds also exceeds a Windows long.
		ageNextSlot(record, std::chrono::minutes(40));
		QVERIFY(!record.addFrame(3 * 1024 * 1024, 1000));
		QCOMPARE(record.iSum, 0);
	}

	void maximumWidthChargesDoNotOverflowTheRollingSum() const {
		BandwidthRecord record;
		const int charge = std::numeric_limits< int >::max();
		for (int i = 0; i < N_BANDWIDTH_SLOTS * 2; ++i) {
			ageNextSlot(record);
			QVERIFY(record.addFrame(charge, charge));
			const qint64 expected = static_cast< qint64 >(std::min(i + 1, N_BANDWIDTH_SLOTS)) * charge;
			QCOMPARE(static_cast< qint64 >(record.iSum), expected);
		}
	}

	void displayedBandwidthClampsInsteadOfWrapping() const {
		BandwidthRecord record;
		for (Timer &timer : record.a_qtWhen)
			timer = ElapsedTimer(std::chrono::seconds(2));
		for (int i = 0; i < 3; ++i) {
			ageNextSlot(record);
			QVERIFY(record.addFrame(std::numeric_limits< int >::max(), std::numeric_limits< int >::max()));
			record.a_qtWhen[i] = ElapsedTimer(std::chrono::milliseconds(500));
		}
		QCOMPARE(record.bandwidth(), std::numeric_limits< int >::max());
	}

	void voiceSizedChargesKeepTheirExistingBehavior() const {
		BandwidthRecord record;
		for (int i = 0; i < N_BANDWIDTH_SLOTS * 2; ++i) {
			ageNextSlot(record, std::chrono::seconds(1));
			QVERIFY(record.addFrame(1024, 1024 * 1024));
			QCOMPARE(static_cast< qint64 >(record.iSum),
					 static_cast< qint64 >(std::min(i + 1, N_BANDWIDTH_SLOTS)) * 1024);
		}
	}

	void refusedChargeDoesNotAdvanceOrOverwriteAcceptedState() const {
		BandwidthRecord record;
		ageNextSlot(record, std::chrono::seconds(1));
		QVERIFY(record.addFrame(100, 1000));
		const auto sum = record.iSum;
		const int slot = record.iRecNum;
		ageNextSlot(record, std::chrono::seconds(1));
		QVERIFY(!record.addFrame(1024 * 1024, 1000));
		QCOMPARE(record.iSum, sum);
		QCOMPARE(record.iRecNum, slot);
	}

	void invalidChargesCannotReduceOrBypassTheBudget() const {
		BandwidthRecord record;
		ageNextSlot(record);
		QVERIFY(!record.addFrame(-1, 1000));
		QVERIFY(!record.addFrame(0, 1000));
		QVERIFY(!record.addFrame(1, 0));
		QVERIFY(!record.addFrame(1, -1));
		QCOMPARE(record.iSum, 0);
		QCOMPARE(record.iRecNum, 0);
	}
};

QTEST_APPLESS_MAIN(TestBandwidthRecord)
#include "TestBandwidthRecord.moc"
