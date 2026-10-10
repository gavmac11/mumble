// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/crypto/Merkle.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

#include <QIODevice>

#include <limits>

namespace PQFT {

QByteArray merkleRootFromDevice(QIODevice &source, quint32 chunkSize, quint64 chunkCount,
								const std::function< bool() > &isCancelled) {
	if ((isCancelled && isCancelled()) || chunkSize == 0 || chunkSize > std::numeric_limits< int >::max()
		|| chunkCount > static_cast< quint64 >(std::numeric_limits< int >::max()) || !source.isReadable())
		return {};
	QVector< QByteArray > digests;
	digests.reserve(static_cast< int >(chunkCount));
	while (!source.atEnd()) {
		if (isCancelled && isCancelled())
			return {};
		const QByteArray chunk = source.read(chunkSize);
		if (chunk.isEmpty() || static_cast< quint64 >(digests.size()) >= chunkCount)
			return {};
		digests.append(merkleChunkHash(chunk));
	}
	if (static_cast< quint64 >(digests.size()) != chunkCount)
		return {};
	return merkleRoot(std::move(digests), isCancelled);
}

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

QByteArray merkleRoot(QVector< QByteArray > chunkHashes, const std::function< bool() > &isCancelled) {
	if (isCancelled && isCancelled())
		return QByteArray();
	for (const QByteArray &hash : chunkHashes) {
		if ((isCancelled && isCancelled()) || hash.size() != HashSize)
			return QByteArray();
	}

	if (chunkHashes.isEmpty())
		return sha384({ QByteArray() });

	// Leaves
	QVector< QByteArray > level;
	level.reserve(chunkHashes.size());
	for (const QByteArray &chunkHash : chunkHashes) {
		if (isCancelled && isCancelled())
			return QByteArray();
		level.append(merkleLeaf(chunkHash));
	}

	// Pair up until a single root remains; duplicate the last entry on odd
	// levels so the shape depends only on the count.
	while (level.size() > 1) {
		QVector< QByteArray > next;
		next.reserve((level.size() + 1) / 2);
		for (qsizetype i = 0; i < level.size(); i += 2) {
			if (isCancelled && isCancelled())
				return QByteArray();
			const QByteArray &left  = level.at(i);
			const QByteArray &right = (i + 1 < level.size()) ? level.at(i + 1) : level.at(i);
			next.append(merkleNode(left, right));
		}
		level = std::move(next);
	}

	return level.first();
}

} // namespace PQFT
