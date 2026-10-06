// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/Merkle.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

namespace PQFT {

QByteArray merkleChunkHash(const QByteArray &chunk) {
	return sha384({ chunk });
}

QByteArray merkleLeaf(const QByteArray &chunkHash) {
	const QByteArray prefix(1, '\x00');
	return sha384({ prefix, chunkHash });
}

QByteArray merkleNode(const QByteArray &left, const QByteArray &right) {
	const QByteArray prefix(1, '\x01');
	return sha384({ prefix, left, right });
}

QByteArray merkleRoot(QVector< QByteArray > chunkHashes) {
	for (const QByteArray &hash : chunkHashes) {
		if (hash.size() != HashSize)
			return QByteArray();
	}

	if (chunkHashes.isEmpty())
		return sha384({ QByteArray() });

	// Leaves
	QVector< QByteArray > level;
	level.reserve(chunkHashes.size());
	for (const QByteArray &chunkHash : chunkHashes) {
		level.append(merkleLeaf(chunkHash));
	}

	// Pair up until a single root remains; duplicate the last entry on odd
	// levels so the shape depends only on the count.
	while (level.size() > 1) {
		QVector< QByteArray > next;
		next.reserve((level.size() + 1) / 2);
		for (qsizetype i = 0; i < level.size(); i += 2) {
			const QByteArray &left  = level.at(i);
			const QByteArray &right = (i + 1 < level.size()) ? level.at(i + 1) : level.at(i);
			next.append(merkleNode(left, right));
		}
		level = std::move(next);
	}

	return level.first();
}

} // namespace PQFT
