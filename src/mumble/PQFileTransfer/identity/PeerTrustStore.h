// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// TOFU pinning of file-transfer identity fingerprints, keyed by
// (server certificate digest, username). A changed fingerprint is a hard
// block until the user explicitly re-verifies (PROTOCOL.md §8).

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_PEERTRUSTSTORE_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_PEERTRUSTSTORE_H_

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace PQFT {

struct PinnedPeer {
	QByteArray serverDigest;
	QString username;
	QByteArray fingerprint;
	QString safetyNumber; // safety number against the local identity at pin time
	QDate firstSeen;
	bool verified = false;
};

enum class TrustState {
	/// No pin yet (first contact).
	NewPeer,
	/// Pinned (TOFU) but not yet verified out-of-band.
	Pinned,
	/// Pinned and verified via safety number.
	Verified,
	/// Pinned fingerprint differs from the presented one — hard block.
	Changed,
};

class PeerTrustStore {
public:
	explicit PeerTrustStore(QSqlDatabase db);

	/// Non-mutating check of the presented fingerprint.
	TrustState check(const QByteArray &serverDigest, const QString &username,
					 const QByteArray &peerFingerprint, const QString &safetyNumber) const;

	/// Check and pin on first contact (TOFU). Never overwrites an existing
	/// pin — a mismatch returns Changed and stores nothing.
	TrustState checkAndPin(const QByteArray &serverDigest, const QString &username,
						   const QByteArray &peerFingerprint, const QString &safetyNumber);

	/// Mark the pinned fingerprint as verified out-of-band.
	bool markVerified(const QByteArray &serverDigest, const QString &username, const QByteArray &expectedFingerprint);

	/// Drop the pin entirely (used by the explicit re-verification flow).
	bool removePin(const QByteArray &serverDigest, const QString &username);

	/// Optional querySucceeded distinguishes an absent pin from storage/fetch failure.
	bool lookup(PinnedPeer &out, const QByteArray &serverDigest, const QString &username,
				bool *querySucceeded = nullptr) const;

	QList< PinnedPeer > list() const;

private:
	QSqlDatabase m_db;
};

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_IDENTITY_PEERTRUSTSTORE_H_
