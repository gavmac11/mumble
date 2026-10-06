// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Qt 6.4's QCborValue::toCbor(SortKeysInMaps) does not reliably sort map keys,
// so canonical encoding is done by a small purpose-built encoder over the
// restricted value set (Integer, ByteArray, String, Bool, Array, Map).
// Canonical form (RFC 8949 §4.2.1 flavor): definite lengths only, shortest-form
// integer heads, map keys sorted bytewise on their encoded form, no duplicate
// keys, no tags, no floats.

#include "PQFileTransfer/crypto/CanonicalCBOR.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborStreamReader>

#include <algorithm>

namespace PQFT {
namespace {

void appendHead(QByteArray &out, quint8 majorType, quint64 value) {
	if (value < 24) {
		out.append(static_cast< char >((majorType << 5) | static_cast< quint8 >(value)));
	} else if (value <= 0xff) {
		out.append(static_cast< char >((majorType << 5) | 24));
		out.append(static_cast< char >(value));
	} else if (value <= 0xffff) {
		out.append(static_cast< char >((majorType << 5) | 25));
		out.append(static_cast< char >(value >> 8));
		out.append(static_cast< char >(value));
	} else if (value <= 0xffffffff) {
		out.append(static_cast< char >((majorType << 5) | 26));
		out.append(static_cast< char >(value >> 24));
		out.append(static_cast< char >(value >> 16));
		out.append(static_cast< char >(value >> 8));
		out.append(static_cast< char >(value));
	} else {
		out.append(static_cast< char >((majorType << 5) | 27));
		for (int shift = 56; shift >= 0; shift -= 8) {
			out.append(static_cast< char >((value >> shift) & 0xff));
		}
	}
}

bool encodeValue(QByteArray &out, const QCborValue &value);

bool encodeMap(QByteArray &out, const QCborMap &map) {
	// Sort entries by the encoded form of the key (bytewise). Duplicate keys
	// after sorting mean the map is not encodable canonically.
	struct Entry {
		QByteArray encodedKey;
		QCborValue value;
	};
	QVector< Entry > entries;
	entries.reserve(map.size());
	for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
		QByteArray encodedKey;
		if (!encodeValue(encodedKey, it.key()))
			return false;
		entries.append({ encodedKey, it.value() });
	}
	std::sort(entries.begin(), entries.end(),
			  [](const Entry &a, const Entry &b) { return a.encodedKey < b.encodedKey; });
	for (int i = 1; i < entries.size(); ++i) {
		if (entries.at(i).encodedKey == entries.at(i - 1).encodedKey)
			return false;
	}

	appendHead(out, 5, static_cast< quint64 >(entries.size()));
	for (const Entry &entry : entries) {
		out.append(entry.encodedKey);
		if (!encodeValue(out, entry.value))
			return false;
	}
	return true;
}

bool encodeValue(QByteArray &out, const QCborValue &value) {
	switch (value.type()) {
		case QCborValue::Integer: {
			const qint64 v = value.toInteger();
			if (v >= 0) {
				appendHead(out, 0, static_cast< quint64 >(v));
			} else {
				// Negative integers: major type 1 encodes -1 - n
				appendHead(out, 1, static_cast< quint64 >(-1 - v));
			}
			return true;
		}
		case QCborValue::ByteArray: {
			const QByteArray bytes = value.toByteArray();
			appendHead(out, 2, static_cast< quint64 >(bytes.size()));
			out.append(bytes);
			return true;
		}
		case QCborValue::String: {
			const QString string = value.toString();
			const QByteArray utf8 = string.toUtf8();
			appendHead(out, 3, static_cast< quint64 >(utf8.size()));
			out.append(utf8);
			return true;
		}
		case QCborValue::False:
			out.append(static_cast< char >(0xf4));
			return true;
		case QCborValue::True:
			out.append(static_cast< char >(0xf5));
			return true;
		case QCborValue::Array: {
			const QCborArray array = value.toArray();
			appendHead(out, 4, static_cast< quint64 >(array.size()));
			for (const QCborValue &element : array) {
				if (!encodeValue(out, element))
					return false;
			}
			return true;
		}
		case QCborValue::Map:
			return encodeMap(out, value.toMap());
		default:
			return false;
	}
}

} // namespace

bool isAllowedValue(const QCborValue &value) {
	switch (value.type()) {
		case QCborValue::Integer:
		case QCborValue::ByteArray:
		case QCborValue::False:
		case QCborValue::True:
		case QCborValue::String:
			return true;
		case QCborValue::Array: {
			const QCborArray array = value.toArray();
			for (const QCborValue &element : array) {
				if (!isAllowedValue(element))
					return false;
			}
			return true;
		}
		case QCborValue::Map: {
			const QCborMap map = value.toMap();
			for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
				if (!isAllowedValue(it.key()) || !isAllowedValue(it.value()))
					return false;
			}
			return true;
		}
		default:
			return false;
	}
}

QByteArray encodeCanonical(const QCborValue &value) {
	if (!isAllowedValue(value))
		return QByteArray();

	QByteArray out;
	if (!encodeValue(out, value)) {
		return QByteArray();
	}
	return out;
}

QByteArray encodeCanonicalMap(const QVector< QPair< QCborValue, QCborValue > > &entries) {
	QCborMap map;
	for (const auto &entry : entries) {
		if (!isAllowedValue(entry.first) || !isAllowedValue(entry.second)) {
			return QByteArray();
		}
		if (map.contains(entry.first)) {
			return QByteArray();
		}
		map.insert(entry.first, entry.second);
	}
	return encodeCanonical(map);
}

bool decodeCanonical(const QByteArray &bytes, QCborValue &out) {
	out = QCborValue();
	if (bytes.isEmpty())
		return false;

	QCborParserError error;
	const QCborValue value = QCborValue::fromCbor(bytes, &error);
	if (error.error != QCborError::NoError)
		return false;
	if (!isAllowedValue(value))
		return false;

	// Reject anything not already in canonical form: re-encode and require
	// byte equality. This rejects trailing bytes, non-shortest integers,
	// indefinite lengths, unsorted or duplicate map keys and disallowed types.
	const QByteArray canonical = encodeCanonical(value);
	if (canonical != bytes)
		return false;

	out = value;
	return true;
}

} // namespace PQFT
