// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/Argon2Wrap.h"

#include <argon2.h>

namespace PQFT {

bool Argon2Params::withinBounds() const {
	return timeCost >= Argon2MinTimeCost && timeCost <= Argon2MaxTimeCost
		   && memoryKiB >= Argon2MinMemoryKiB && memoryKiB <= Argon2MaxMemoryKiB
		   && parallelism >= Argon2MinParallelism && parallelism <= Argon2MaxParallelism;
}

bool argon2idDerive(SecureBytes &output, QByteArray &password, const QByteArray &salt,
					const Argon2Params &params) {
	output.clear();
	if (!params.withinBounds() || salt.size() < 16 || salt.size() > 64 || password.isEmpty())
		return false;

	output = SecureBytes(Argon2OutputSize);
	const int status = argon2id_hash_raw(
		params.timeCost, params.memoryKiB, params.parallelism, password.constData(),
		static_cast< size_t >(password.size()), salt.constData(), static_cast< size_t >(salt.size()),
		output.data(), static_cast< size_t >(output.size()));

	zeroize(password);
	if (status != ARGON2_OK) {
		output.clear();
		return false;
	}
	return true;
}

} // namespace PQFT
