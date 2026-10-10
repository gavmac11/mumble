// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "BandwidthRecord.h"

#include <QtCore/QMutexLocker>

#include <algorithm>
#include <chrono>
#include <limits>

static_assert(N_BANDWIDTH_SLOTS > 0
			  && N_BANDWIDTH_SLOTS
					 <= std::numeric_limits< qint64 >::max() / std::numeric_limits< int >::max() / 1000000LL);

BandwidthRecord::BandwidthRecord() {
	iRecNum = 0;
	iSum    = 0;
	for (int i = 0; i < N_BANDWIDTH_SLOTS; i++)
		a_iBW[i] = 0;
}

bool BandwidthRecord::addFrame(int size, int maxpersec) {
	QMutexLocker ml(&qmMutex);

	if (size <= 0 || maxpersec <= 0)
		return false;

	const qint64 elapsed = a_qtWhen[iRecNum].elapsed().count();
	if (elapsed <= 0)
		return false;

	const qint64 nsum = iSum - a_iBW[iRecNum] + size;
	// At most N_BANDWIDTH_SLOTS positive int charges: the scaled sum fits qint64.
	// Keep the elapsed time and multiplication wide on Windows as well as Unix.
	const qint64 bw = (nsum * 1000000LL) / elapsed;

	if (bw > maxpersec)
		return false;

	a_iBW[iRecNum] = size;
	a_qtWhen[iRecNum].restart();

	iSum = nsum;

	iRecNum++;
	if (iRecNum == N_BANDWIDTH_SLOTS)
		iRecNum = 0;

	return true;
}

int BandwidthRecord::onlineSeconds() const {
	QMutexLocker ml(&qmMutex);

	return static_cast< int >(tFirst.elapsed< std::chrono::seconds >().count());
}

int BandwidthRecord::idleSeconds() const {
	QMutexLocker ml(&qmMutex);

	std::chrono::microseconds iIdle = a_qtWhen[(iRecNum + N_BANDWIDTH_SLOTS - 1) % N_BANDWIDTH_SLOTS].elapsed();
	if (tIdleControl.elapsed() < iIdle)
		iIdle = tIdleControl.elapsed();

	return static_cast< int >(std::chrono::duration_cast< std::chrono::seconds >(iIdle).count());
}

void BandwidthRecord::resetIdleSeconds() {
	QMutexLocker ml(&qmMutex);

	tIdleControl.restart();
}

int BandwidthRecord::bandwidth() const {
	QMutexLocker ml(&qmMutex);

	qint64 sum = 0;
	std::chrono::microseconds elapsed{ 0 };

	for (int i = 1; i < N_BANDWIDTH_SLOTS; ++i) {
		int idx                     = (iRecNum + N_BANDWIDTH_SLOTS - i) % N_BANDWIDTH_SLOTS;
		std::chrono::microseconds e = a_qtWhen[idx].elapsed();
		if (e > std::chrono::seconds(1)) {
			break;
		} else {
			sum += a_iBW[idx];
			elapsed = e;
		}
	}

	if (elapsed < std::chrono::milliseconds(250))
		return 0;

	const qint64 bandwidth = (sum * 1000000LL) / elapsed.count();
	return static_cast< int >(std::min(bandwidth, static_cast< qint64 >(std::numeric_limits< int >::max())));
}
