// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Argon2id wrapper for the file-password layer (PQShield v2 §10).

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_ARGON2WRAP_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_ARGON2WRAP_H_

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

class QString;

namespace PQFT {

/// Argon2id cost parameters as they travel (signed) inside the manifest.
/// Receivers enforce MIN ≤ params ≤ MAX before deriving anything.
struct Argon2Params {
	quint32 timeCost     = Argon2DefaultTimeCost;
	quint32 memoryKiB    = Argon2DefaultMemoryKiB;
	quint32 parallelism  = Argon2DefaultParallelism;

	bool operator==(const Argon2Params &other) const = default;

	/// True when within the accepted [MIN, MAX] window (fail closed both ways).
	bool withinBounds() const;
};

/// Derive a 32-byte key from password + salt. Returns false on failure.
/// The password is zeroized after use.
bool argon2idDerive(SecureBytes &output, QByteArray &password, const QByteArray &salt,
					const Argon2Params &params);

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_ARGON2WRAP_H_
