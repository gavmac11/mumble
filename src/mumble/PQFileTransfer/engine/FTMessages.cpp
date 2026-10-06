// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FTMessages.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"

namespace PQFT {
namespace FTFrame {

QByteArray encodeHandshake(quint8 type, const QByteArray &canonicalBody) {
	if (type < 1 || type > 4 || canonicalBody.isEmpty())
		return QByteArray();
	QByteArray frame;
	frame.reserve(1 + canonicalBody.size());
	frame.append(static_cast< char >((Version << 4) | (type & 0x0f)));
	frame.append(canonicalBody);
	return frame;
}

QByteArray encodeControl(quint8 type, quint64 seq, const QByteArray &ciphertextAndTag) {
	if (type < TypeManifest || type > TypeAbort)
		return QByteArray();
	QByteArray frame;
	frame.reserve(9 + ciphertextAndTag.size());
	frame.append(static_cast< char >((Version << 4) | (type & 0x0f)));
	frame.append(uint64be(seq));
	frame.append(ciphertextAndTag);
	return frame;
}

bool decodeHeader(const QByteArray &frame, quint8 &type) {
	if (frame.size() < 1)
		return false;
	const quint8 header = static_cast< quint8 >(frame.at(0));
	if ((header >> 4) != Version)
		return false;
	const quint8 t = header & 0x0f;
	if (t < 1 || t > TypeAbort)
		return false;
	type = t;
	return true;
}

QByteArray controlAAD(const QByteArray &frame) {
	if (frame.size() < 9)
		return QByteArray();
	return frame.left(9);
}

QByteArray controlNonce(const QByteArray &prefix, quint64 seq) {
	QByteArray nonce;
	nonce.reserve(NonceSize);
	nonce.append(prefix.left(4));
	nonce.append(uint64be(seq));
	return nonce.size() == NonceSize ? nonce : QByteArray();
}

bool decodeControl(const QByteArray &frame, quint64 &seq, QByteArray &ciphertextAndTag) {
	quint8 type = 0;
	if (!decodeHeader(frame, type) || type < TypeManifest || frame.size() < 9 + TagSize)
		return false;

	const unsigned char *p = reinterpret_cast< const unsigned char * >(frame.constData()) + 1;
	quint64 value = 0;
	for (int i = 0; i < 8; ++i) {
		value = (value << 8) | p[i];
	}
	seq				   = value;
	ciphertextAndTag   = frame.mid(9);
	return true;
}

} // namespace FTFrame
} // namespace PQFT
