// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FTManifest.h"

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/identity/FTIdentity.h"

#include <QCborMap>
#include <QPair>
#include <QString>
#include <QVector>

namespace PQFT {
namespace {

using MapEntries = QVector< QPair< QCborValue, QCborValue > >;

// Manifest CBOR keys (fixed order)
constexpr int KVersion	= 1;
constexpr int KSuite		 = 2;
constexpr int KTransferId	= 3;
constexpr int KFpA			 = 4;
constexpr int KFpB			 = 5;
constexpr int KFileName	 = 6;
constexpr int KMime		 = 7;
constexpr int KFileSize	 = 8;
constexpr int KChunkSize	 = 9;
constexpr int KChunkCount	= 10;
constexpr int KMerkleRoot   = 11;
constexpr int KPasswordMode = 12;
constexpr int KArgon2Params = 13;
constexpr int KArgon2Salt   = 14;
constexpr int KWrappedKey   = 15;
constexpr int KCreatedAt	= 16;
constexpr int KSignature	= 17;

// Argon2 params map keys
constexpr int KA2Time = 1;
constexpr int KA2Mem  = 2;
constexpr int KA2Par  = 3;

void addEntry(MapEntries &entries, int key, const QCborValue &value) {
	entries.append(qMakePair(QCborValue(key), value));
}

QCborMap argon2ParamsMap(const Argon2Params &params) {
	QCborMap map;
	map.insert(QCborValue(KA2Time), QCborValue(static_cast< qint64 >(params.timeCost)));
	map.insert(QCborValue(KA2Mem), QCborValue(static_cast< qint64 >(params.memoryKiB)));
	map.insert(QCborValue(KA2Par), QCborValue(static_cast< qint64 >(params.parallelism)));
	return map;
}

MapEntries manifestEntries(const FTManifest &m, bool withSignature) {
	MapEntries e;
	addEntry(e, KVersion, QCborValue(static_cast< int >(Version)));
	addEntry(e, KSuite, QCborValue(QString::fromLatin1(SuiteId)));
	addEntry(e, KTransferId, QCborValue(m.transferId));
	addEntry(e, KFpA, QCborValue(m.fpA));
	addEntry(e, KFpB, QCborValue(m.fpB));
	addEntry(e, KFileName, QCborValue(m.fileName));
	addEntry(e, KMime, QCborValue(m.mimeType));
	addEntry(e, KFileSize, QCborValue(static_cast< qint64 >(m.fileSize)));
	addEntry(e, KChunkSize, QCborValue(static_cast< qint64 >(m.chunkSize)));
	addEntry(e, KChunkCount, QCborValue(static_cast< qint64 >(m.chunkCount)));
	addEntry(e, KMerkleRoot, QCborValue(m.merkleRoot));
	addEntry(e, KPasswordMode, QCborValue(m.passwordMode));
	if (m.passwordMode) {
		addEntry(e, KArgon2Params, QCborValue(argon2ParamsMap(*m.argon2)));
		addEntry(e, KArgon2Salt, QCborValue(m.argon2Salt));
	}
	addEntry(e, KWrappedKey, QCborValue(m.wrappedFileKey));
	addEntry(e, KCreatedAt, QCborValue(static_cast< qint64 >(m.createdAtUnix)));
	if (withSignature) {
		addEntry(e, KSignature, QCborValue(m.signature));
	}
	return e;
}

MapEntries aadEntries(const QByteArray &transferId, const QByteArray &transferDigest,
					  quint64 index, quint64 total, quint32 length) {
	MapEntries e;
	e.append(qMakePair(QCborValue(1), QCborValue(transferId)));
	e.append(qMakePair(QCborValue(2), QCborValue(transferDigest)));
	e.append(qMakePair(QCborValue(3), QCborValue(static_cast< qint64 >(index))));
	e.append(qMakePair(QCborValue(4), QCborValue(static_cast< qint64 >(total))));
	e.append(qMakePair(QCborValue(5), QCborValue(static_cast< qint64 >(length))));
	e.append(qMakePair(QCborValue(6), QCborValue(QString::fromLatin1(SuiteId))));
	return e;
}

QByteArray aad1(const QByteArray &transferId, bool passwordMode, const QByteArray &fpA,
				const QByteArray &fpB) {
	MapEntries e;
	e.append(qMakePair(QCborValue(1), QCborValue(transferId)));
	e.append(qMakePair(QCborValue(2), QCborValue(passwordMode)));
	e.append(qMakePair(QCborValue(3), QCborValue(QString::fromLatin1(SuiteId))));
	e.append(qMakePair(QCborValue(4), QCborValue(fpA)));
	e.append(qMakePair(QCborValue(5), QCborValue(fpB)));
	return encodeCanonicalMap(e);
}

QByteArray aad2(const QByteArray &transferId, bool passwordMode, const QByteArray &fpA,
				const QByteArray &fpB, const Argon2Params &params, const QByteArray &salt) {
	MapEntries e;
	e.append(qMakePair(QCborValue(1), QCborValue(transferId)));
	e.append(qMakePair(QCborValue(2), QCborValue(passwordMode)));
	e.append(qMakePair(QCborValue(3), QCborValue(QString::fromLatin1(SuiteId))));
	e.append(qMakePair(QCborValue(4), QCborValue(fpA)));
	e.append(qMakePair(QCborValue(5), QCborValue(fpB)));
	e.append(qMakePair(QCborValue(6), QCborValue(argon2ParamsMap(params))));
	e.append(qMakePair(QCborValue(7), QCborValue(salt)));
	return encodeCanonicalMap(e);
}

} // namespace

QByteArray manifestBody(const FTManifest &manifest) {
	return encodeCanonicalMap(manifestEntries(manifest, false));
}

QByteArray encodeManifest(const FTManifest &manifest) {
	return encodeCanonicalMap(manifestEntries(manifest, true));
}

QByteArray FTManifest::transferDigest() const {
	MapEntries e;
	e.append(qMakePair(QCborValue(KVersion), QCborValue(Version)));
	e.append(qMakePair(QCborValue(KSuite), QCborValue(QString::fromLatin1(SuiteId))));
	e.append(qMakePair(QCborValue(KTransferId), QCborValue(transferId)));
	e.append(qMakePair(QCborValue(KFpA), QCborValue(fpA)));
	e.append(qMakePair(QCborValue(KFileName), QCborValue(fileName)));
	e.append(qMakePair(QCborValue(KMime), QCborValue(mimeType)));
	e.append(qMakePair(QCborValue(KFileSize), QCborValue(static_cast< qint64 >(fileSize))));
	e.append(qMakePair(QCborValue(KChunkSize), QCborValue(static_cast< qint64 >(chunkSize))));
	e.append(qMakePair(QCborValue(KChunkCount), QCborValue(static_cast< qint64 >(chunkCount))));
	e.append(qMakePair(QCborValue(KMerkleRoot), QCborValue(merkleRoot)));
	e.append(qMakePair(QCborValue(KPasswordMode), QCborValue(passwordMode)));
	// Deliberately NOT part of the digest:
	//  - createdAtUnix: informational only (PQShield v2 §11 note); both sides
	//    must compute the digest identically without depending on clocks.
	//  - Argon2 params + salt: they are signed manifest fields and are bound
	//    into the password-wrap AAD2; the password_mode flag above already
	//    anchors the mode into every chunk.
	return sha384({ encodeCanonicalMap(e) });
}

bool signManifest(FTManifest &manifest,
				  const std::function< bool(QByteArray &, const QByteArray &, const QByteArray &) > &sign) {
	const QByteArray body = manifestBody(manifest);
	if (body.isEmpty())
		return false;
	return sign(manifest.signature, sha384({ body }), QByteArray(LabelManifestCtx));
}

bool parseAndVerifyManifest(FTManifest &out, const QByteArray &encoded,
							const QByteArray &senderFingerprint, quint64 maxFileSize,
							const std::function< bool(const QByteArray &, const QByteArray &,
													  const QByteArray &)> &verify) {
	QCborValue value;
	if (!decodeCanonical(encoded, value) || !value.isMap())
		return false;
	const QCborMap m = value.toMap();

	const bool passwordMode = m.value(QCborValue(KPasswordMode)).isTrue();
	// 15 fields without a password; +2 (Argon2 params, salt) with one
	if (m.size() != (passwordMode ? 17 : 15))
		return false;

	if (m.value(QCborValue(KVersion)).toInteger() != Version)
		return false;
	if (m.value(QCborValue(KSuite)).toString() != QString::fromLatin1(SuiteId))
		return false;

	FTManifest manifest;
	manifest.passwordMode = passwordMode;
	manifest.transferId = m.value(QCborValue(KTransferId)).toByteArray();
	manifest.fpA		= m.value(QCborValue(KFpA)).toByteArray();
	manifest.fpB		= m.value(QCborValue(KFpB)).toByteArray();
	manifest.fileName   = m.value(QCborValue(KFileName)).toString();
	manifest.mimeType   = m.value(QCborValue(KMime)).toString();
	manifest.fileSize   = static_cast< quint64 >(m.value(QCborValue(KFileSize)).toInteger());
	manifest.chunkSize  = static_cast< quint32 >(m.value(QCborValue(KChunkSize)).toInteger());
	manifest.chunkCount = static_cast< quint64 >(m.value(QCborValue(KChunkCount)).toInteger());
	manifest.merkleRoot = m.value(QCborValue(KMerkleRoot)).toByteArray();
	manifest.wrappedFileKey = m.value(QCborValue(KWrappedKey)).toByteArray();
	manifest.createdAtUnix  = static_cast< quint64 >(m.value(QCborValue(KCreatedAt)).toInteger());
	manifest.signature	  = m.value(QCborValue(KSignature)).toByteArray();

	if (manifest.transferId.size() != TransferIdSize || manifest.fpA.size() != HashSize
		|| manifest.fpB.size() != HashSize || manifest.merkleRoot.size() != HashSize)
		return false;

	if (manifest.passwordMode) {
		const QCborMap params = m.value(QCborValue(KArgon2Params)).toMap();
		Argon2Params p;
		p.timeCost	= static_cast< quint32 >(params.value(QCborValue(KA2Time)).toInteger());
		p.memoryKiB  = static_cast< quint32 >(params.value(QCborValue(KA2Mem)).toInteger());
		p.parallelism = static_cast< quint32 >(params.value(QCborValue(KA2Par)).toInteger());
		if (!p.withinBounds())
			return false;
		manifest.argon2	 = p;
		manifest.argon2Salt = m.value(QCborValue(KArgon2Salt)).toByteArray();
		if (manifest.argon2Salt.size() != Argon2SaltSize)
			return false;
	}

	// Sizes and shape
	if (manifest.fileSize == 0 || manifest.fileSize > maxFileSize)
		return false;
	if (manifest.chunkSize < static_cast< quint32 >(MinChunkSize)
		|| manifest.chunkSize > static_cast< quint32 >(MaxChunkSize))
		return false;
	const quint64 expectedChunks = (manifest.fileSize + manifest.chunkSize - 1) / manifest.chunkSize;
	if (manifest.chunkCount != expectedChunks)
		return false;
	const qsizetype expectedWrapped = manifest.passwordMode ? KeySize + TagSize + TagSize
															: KeySize + TagSize;
	if (manifest.wrappedFileKey.size() != expectedWrapped)
		return false;

	// Filename: basename only, no separators, length-capped UTF-8
	const QByteArray nameBytes = manifest.fileName.toUtf8();
	if (nameBytes.isEmpty() || nameBytes.size() > MaxFileNameBytes)
		return false;
	if (manifest.fileName.contains(QLatin1Char('/')) || manifest.fileName.contains(QLatin1Char('\\'))
		|| manifest.fileName == QLatin1String("..") || manifest.fileName == QLatin1String("."))
		return false;

	// The sender identity must be exactly the one the session pinned.
	if (manifest.fpA != senderFingerprint)
		return false;

	// Signature over the sig-less body — verified before any Argon2 work.
	FTManifest unsignedManifest = manifest;
	unsignedManifest.signature.clear();
	const QByteArray body = manifestBody(unsignedManifest);
	if (body.isEmpty())
		return false;
	if (!verify(sha384({ body }), manifest.signature, QByteArray(LabelManifestCtx)))
		return false;

	out = manifest;
	return true;
}

QByteArray chunkNoncePrefix(const QByteArray &fileKey, const QByteArray &transferId) {
	return hkdfExpand(fileKey, QByteArray(LabelChunkNonce) + transferId, 4);
}

QByteArray chunkAAD(const QByteArray &transferId, const QByteArray &transferDigest, quint64 index,
					quint64 total, quint32 length) {
	return encodeCanonicalMap(aadEntries(transferId, transferDigest, index, total, length));
}

bool encryptChunk(QByteArray &out, const QByteArray &fileKey, const QByteArray &transferId,
				  const QByteArray &transferDigest, quint64 index, quint64 total,
				  const QByteArray &chunk) {
	const QByteArray prefix = chunkNoncePrefix(fileKey, transferId);
	if (prefix.isEmpty())
		return false;
	return aesGcmEncrypt(out, fileKey, prefix + uint64be(index), chunk,
						chunkAAD(transferId, transferDigest, index, total,
								 static_cast< quint32 >(chunk.size())));
}

bool decryptChunk(QByteArray &out, const QByteArray &fileKey, const QByteArray &transferId,
				  const QByteArray &transferDigest, quint64 index, quint64 total,
				  const QByteArray &ciphertextAndTag) {
	const QByteArray prefix = chunkNoncePrefix(fileKey, transferId);
	if (prefix.isEmpty() || ciphertextAndTag.size() < TagSize)
		return false;
	const quint32 length = static_cast< quint32 >(ciphertextAndTag.size() - TagSize);
	return aesGcmDecrypt(out, fileKey, prefix + uint64be(index), ciphertextAndTag,
						chunkAAD(transferId, transferDigest, index, total, length));
}

// --- File-key wrapping ----------------------------------------------------------------

bool wrapFileKeySessionLayer(QByteArray &layer1Ct, const QByteArray &fileKey,
							 const QByteArray &sessionKek, const QByteArray &wrapNonce1,
							 const QByteArray &transferId, bool passwordMode,
							 const QByteArray &fpA, const QByteArray &fpB) {
	return aesGcmEncrypt(layer1Ct, sessionKek, wrapNonce1, fileKey,
						 aad1(transferId, passwordMode, fpA, fpB));
}

bool unwrapFileKeySessionLayer(QByteArray &fileKey, const QByteArray &layer1Ct,
							   const QByteArray &sessionKek, const QByteArray &wrapNonce1,
							   const QByteArray &transferId, bool passwordMode,
							   const QByteArray &fpA, const QByteArray &fpB) {
	return aesGcmDecrypt(fileKey, sessionKek, wrapNonce1, layer1Ct,
						 aad1(transferId, passwordMode, fpA, fpB));
}

QByteArray passwordWrapKey(const QByteArray &pwKey, const QByteArray &transferId,
						   const QByteArray &fpA, const QByteArray &fpB) {
	return hkdfExpand(pwKey, QByteArray(LabelPwWrap) + transferId + fpA + fpB, KeySize);
}

QByteArray wrapNonce2(const QByteArray &pwWrapKey) {
	return hkdfExpand(pwWrapKey, QByteArray(LabelWrapNonce2), NonceSize);
}

bool wrapLayer1WithPassword(QByteArray &layer2Ct, const QByteArray &layer1Ct,
							const QByteArray &pwWrapKey, const QByteArray &transferId,
							bool passwordMode, const QByteArray &fpA, const QByteArray &fpB,
							const Argon2Params &params, const QByteArray &salt) {
	return aesGcmEncrypt(layer2Ct, pwWrapKey, wrapNonce2(pwWrapKey), layer1Ct,
						 aad2(transferId, passwordMode, fpA, fpB, params, salt));
}

bool unwrapPasswordLayer(QByteArray &layer1Ct, const QByteArray &layer2Ct,
						 const QByteArray &pwWrapKey, const QByteArray &transferId,
						 bool passwordMode, const QByteArray &fpA, const QByteArray &fpB,
						 const Argon2Params &params, const QByteArray &salt) {
	return aesGcmDecrypt(layer1Ct, pwWrapKey, wrapNonce2(pwWrapKey), layer2Ct,
						 aad2(transferId, passwordMode, fpA, fpB, params, salt));
}

QString genericDecryptionError() {
	// Deliberately identical for wrong password and corrupted transfer —
	// no oracle (§10, §13).
	return QStringLiteral("Decryption failed: wrong password or corrupted transfer.");
}

} // namespace PQFT
