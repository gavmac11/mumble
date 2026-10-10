// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// SHA-384 Merkle tree over chunk digests (PQShield v2 §9.3, PROTOCOL.md §5):
//   leaf_i = SHA-384(0x00 ‖ chunk_hash_i)
//   node   = SHA-384(0x01 ‖ left ‖ right)
// The last node is duplicated when a level has an odd count. The root of a
// zero-chunk transfer is SHA-384 of the empty string.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_MERKLE_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_MERKLE_H_

#include <QByteArray>
#include <QVector>

#include <functional>

class QIODevice;

namespace PQFT {

/// SHA-384 of one chunk (the leaf input).
QByteArray merkleChunkHash(const QByteArray &chunk);

/// Leaf hash for a chunk digest.
QByteArray merkleLeaf(const QByteArray &chunkHash);

/// Internal node hash.
QByteArray merkleNode(const QByteArray &left, const QByteArray &right);

/// Root over the chunk digests, in order. `chunkHashes` must contain
/// HashSize-sized entries (empty input yields the empty-input root).
/// A cancellation request returns an empty result, including during tree construction.
QByteArray merkleRoot(QVector< QByteArray > chunkHashes, const std::function< bool() > &isCancelled = {});

/// Hash a readable device from its current position, checking cancellation between chunks.
/// Empty results indicate cancellation, invalid sizing, or a chunk count mismatch.
QByteArray merkleRootFromDevice(QIODevice &source, quint32 chunkSize, quint64 chunkCount,
								const std::function< bool() > &isCancelled = {});

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_MERKLE_H_
