// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/identity/PeerTrustStore.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace PQFT {
namespace {

bool exec(QSqlQuery &query, const QString &statement = QString()) {
	const bool ok = statement.isEmpty() ? query.exec() : query.exec(statement);
	if (!ok) {
		qWarning("PeerTrustStore: query failed: %s", qPrintable(query.lastError().text()));
		return false;
	}
	return true;
}

PinnedPeer rowToPeer(const QSqlQuery &query) {
	PinnedPeer peer;
	peer.serverDigest = query.value(0).toByteArray();
	peer.username	 = query.value(1).toString();
	peer.fingerprint = query.value(2).toByteArray();
	peer.safetyNumber = query.value(3).toString();
	peer.firstSeen	  = query.value(4).toDate();
	peer.verified	  = query.value(5).toInt() != 0;
	return peer;
}

} // namespace

PeerTrustStore::PeerTrustStore(QSqlDatabase db) : m_db(std::move(db)) { }

TrustState PeerTrustStore::check(const QByteArray &serverDigest, const QString &username,
								 const QByteArray &peerFingerprint,
								 const QString &safetyNumber) const {
	Q_UNUSED(safetyNumber);

	PinnedPeer pinned;
	if (!lookup(pinned, serverDigest, username)) {
		return TrustState::NewPeer;
	}
	if (pinned.fingerprint != peerFingerprint) {
		return TrustState::Changed;
	}
	return pinned.verified ? TrustState::Verified : TrustState::Pinned;
}

TrustState PeerTrustStore::checkAndPin(const QByteArray &serverDigest, const QString &username,
										const QByteArray &peerFingerprint,
										const QString &safetyNumber) {
	const TrustState state = check(serverDigest, username, peerFingerprint, safetyNumber);
	if (state != TrustState::NewPeer) {
		return state;
	}

	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("INSERT OR REPLACE INTO `ft_pins` "
									 "(`server_digest`, `username`, `peer_fingerprint`, "
									 "`safety_number`, `first_seen`, `verified`) "
									 "VALUES (?, ?, ?, ?, ?, 0)")))
		return TrustState::Changed; // fail closed on storage errors

	query.addBindValue(serverDigest);
	query.addBindValue(username);
	query.addBindValue(peerFingerprint);
	query.addBindValue(safetyNumber);
	query.addBindValue(QDate::currentDate());
	if (!exec(query)) {
		return TrustState::Changed;
	}
	return TrustState::Pinned;
}

bool PeerTrustStore::markVerified(const QByteArray &serverDigest, const QString &username) {
	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("UPDATE `ft_pins` SET `verified` = 1 "
									 "WHERE `server_digest` = ? AND `username` = ?")))
		return false;
	query.addBindValue(serverDigest);
	query.addBindValue(username);
	return exec(query);
}

bool PeerTrustStore::removePin(const QByteArray &serverDigest, const QString &username) {
	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("DELETE FROM `ft_pins` "
									 "WHERE `server_digest` = ? AND `username` = ?")))
		return false;
	query.addBindValue(serverDigest);
	query.addBindValue(username);
	return exec(query);
}

bool PeerTrustStore::lookup(PinnedPeer &out, const QByteArray &serverDigest,
							const QString &username) const {
	QSqlQuery query(m_db);
	if (!query.prepare(QLatin1String("SELECT `server_digest`, `username`, `peer_fingerprint`, "
									 "`safety_number`, `first_seen`, `verified` FROM `ft_pins` "
									 "WHERE `server_digest` = ? AND `username` = ?")))
		return false;
	query.addBindValue(serverDigest);
	query.addBindValue(username);
	if (!exec(query) || !query.next())
		return false;

	out = rowToPeer(query);
	return true;
}

QList< PinnedPeer > PeerTrustStore::list() const {
	QList< PinnedPeer > peers;
	QSqlQuery query(m_db);
	if (!exec(query, QLatin1String("SELECT `server_digest`, `username`, `peer_fingerprint`, "
								   "`safety_number`, `first_seen`, `verified` FROM `ft_pins` "
								   "ORDER BY `username`")))
		return peers;
	while (query.next()) {
		peers.append(rowToPeer(query));
	}
	return peers;
}

} // namespace PQFT
