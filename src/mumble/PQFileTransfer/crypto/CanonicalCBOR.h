// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Deterministic ("canonical") CBOR for handshake frames and manifests
// (PROTOCOL.md §2). Canonical form as produced by
// QCborValue::toCbor(QCborValue::SortKeysInMaps): definite lengths,
// shortest-form integers, map keys sorted by encoded bytes. Allowed value
// types are restricted to Integer, ByteArray, Bool, String, Array and Map
// thereof — Undefined, Null, tags, floats and doubles are rejected.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CANONICALCBOR_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CANONICALCBOR_H_

#include <QByteArray>
#include <QCborValue>
#include <QPair>
#include <QVector>

namespace PQFT {

/// Encode a map canonically. Entries must use only allowed value types;
/// duplicate keys abort encoding (returns empty QByteArray).
QByteArray encodeCanonicalMap(const QVector< QPair< QCborValue, QCborValue > > &entries);

/// Encode any allowed QCborValue canonically (empty QByteArray on
/// disallowed types).
QByteArray encodeCanonical(const QCborValue &value);

/// Strict decode: parses exactly one value covering the whole buffer, then
/// re-encodes it canonically and byte-compares. Any deviation — trailing
/// bytes, non-shortest integers, indefinite lengths, unsorted or duplicate
/// map keys, disallowed types — is rejected.
bool decodeCanonical(const QByteArray &bytes, QCborValue &out);

/// True when `value` contains only allowed types, recursively.
bool isAllowedValue(const QCborValue &value);

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_CRYPTO_CANONICALCBOR_H_
