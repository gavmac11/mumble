// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// Framing of the opaque payloads inside FileTransferControl
// (PROTOCOL.md §4): a one-byte header {version:4, type:4} followed by
// either a canonical-CBOR handshake body (M1–M4) or an AEAD control
// record {u64be seq, ciphertext||tag} whose AAD is header||seq.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMESSAGES_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMESSAGES_H_

#include <QByteArray>
#include <QtGlobal>

namespace PQFT {

namespace FTFrame {
	/// Frame types (low nibble of the header byte).
	enum FrameType : quint8 {
		TypeM1          = 1,
		TypeM2          = 2,
		TypeM3          = 3,
		TypeM4          = 4,
		TypeManifest    = 5,
		TypeComplete    = 6,
		TypeAbort       = 7,
	};

	/// Encode a handshake frame (header + canonical-CBOR body).
	QByteArray encodeHandshake(quint8 type, const QByteArray &canonicalBody);

	/// Encode a control record: header | u64be(seq) | ciphertext||tag.
	/// The AAD passed to the AEAD is header||seq (the first 9 bytes).
	QByteArray encodeControl(quint8 type, quint64 seq, const QByteArray &ciphertextAndTag);

	/// Decode the header byte. Returns false on wrong version or unknown type.
	bool decodeHeader(const QByteArray &frame, quint8 &type);

	/// The 9 AAD bytes of a control frame (header||seq).
	QByteArray controlAAD(const QByteArray &frame);

	/// The nonce for a control record: 4-byte direction prefix || u64be(seq).
	QByteArray controlNonce(const QByteArray &prefix, quint64 seq);

	/// Split a control frame: seq out, ciphertext||tag out.
	bool decodeControl(const QByteArray &frame, quint64 &seq, QByteArray &ciphertextAndTag);
} // namespace FTFrame

} // namespace PQFT

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTMESSAGES_H_
