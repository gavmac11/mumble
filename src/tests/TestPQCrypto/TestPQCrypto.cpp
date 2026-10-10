// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QTest>
#include <QBuffer>

#include "PQFileTransfer/PQFTConstants.h"
#include "PQFileTransfer/crypto/Argon2Wrap.h"
#include "PQFileTransfer/crypto/CanonicalCBOR.h"
#include "PQFileTransfer/crypto/CryptoUtils.h"
#include "PQFileTransfer/crypto/KemMLKEM768.h"
#include "PQFileTransfer/crypto/Merkle.h"
#include "PQFileTransfer/crypto/SigMLDSA65.h"

#include <argon2.h>

#include <QCborValue>
#include <cstring>

namespace {
QByteArray fromHex(const char *hex) {
	return QByteArray::fromHex(QByteArray(hex));
}
} // namespace

class TestPQCrypto : public QObject {
	Q_OBJECT
private slots:
	// --- Hashing ---------------------------------------------------------
	void sha384Vector();
	void hmacSha384Vector();
	void constantTimeCompare();

	// --- HKDF --------------------------------------------------------------
	void hkdfVectors();

	// --- X25519 --------------------------------------------------------------
	void x25519Rfc7748();
	void x25519RoundTrip();
	void x25519SmallOrderRejected();

	// --- AES-256-GCM -----------------------------------------------------------
	void aesGcmNistVector();
	void aesGcmTamperFails();
	void aesGcmEmptyPlaintext();

	// --- ML-KEM-768 ------------------------------------------------------------
	void mlkemRoundTrip();
	void mlkemBadCiphertext();

	// --- ML-DSA-65 ----------------------------------------------------------------
	void mldsaSignVerify();
	void mldsaTamperFails();
	void mldsaContextSeparated();

	// --- Argon2id --------------------------------------------------------------------
	void argon2Vector();
	void argon2Bounds();

	// --- Canonical CBOR -----------------------------------------------------------------
	void cborEncodeVector();
	void cborRejectsNonCanonical();
	void cborRejectsDuplicateKeys();

	// --- Merkle -------------------------------------------------------------------------
	void merkleVectors();
	void merkleRejectsBadLeaf();
	void merkleCancellation_data();
	void merkleCancellation();
	void deviceHashCancellation();
	void deviceHashRejectsWrongCount();
};

void TestPQCrypto::sha384Vector() {
	// FIPS 180-4: SHA-384("abc")
	QByteArray out = PQFT::sha384({ QByteArray("abc") });
	QCOMPARE(out.size(), 48);
	QCOMPARE(out.toHex(),
			 QByteArray("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
						"8086072ba1e7cc2358baeca134c825a7"));

	// Concatenation: SHA-384("ab" || "c") must equal SHA-384("abc")
	QByteArray parts = PQFT::sha384({ QByteArray("ab"), QByteArray("c") });
	QCOMPARE(parts, out);
}

void TestPQCrypto::hmacSha384Vector() {
	// RFC 4231 test case 1
	QByteArray key(20, '\x0b');
	QByteArray out = PQFT::hmacSha384(key, { QByteArray("Hi There") });
	QCOMPARE(out.toHex(),
			 QByteArray("afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
						"faea9ea9076ede7f4af152e8b2fa9cb6"));
}

void TestPQCrypto::constantTimeCompare() {
	QByteArray a = PQFT::randomBytes(32);
	QByteArray b = a;
	QVERIFY(PQFT::constantTimeEquals(a, b));
	b[10] = b[10] ^ 0x01;
	QVERIFY(!PQFT::constantTimeEquals(a, b));
	QVERIFY(!PQFT::constantTimeEquals(a, QByteArray(a.size() - 1, 0)));
}

void TestPQCrypto::hkdfVectors() {
	// RFC 5869 structure with SHA-384; expected values computed with an
	// independent implementation (Python hmac) and pinned.
	const QByteArray salt("salt");
	const QByteArray ikm("ikm");
	const QByteArray info("info");

	const QByteArray prk = PQFT::hkdfExtract(salt, ikm);
	QCOMPARE(prk.toHex(),
			 QByteArray("d2be1d1d8a6a32a6e02ff57e1a1d79658aed17eac0a36729c8b1324e90a18fda"
						"759f02e3ee851fa84057188bd107f282"));

	const QByteArray okm = PQFT::hkdfExpand(prk, info, 42);
	QCOMPARE(okm.size(), 42);
	QCOMPARE(okm.toHex(),
			 QByteArray("8a4904829f7acb5fe62bfbce3ed1a2d9428bdcba65d4db11e7471f3b7ab9eaff"
						"b058cfdad0c509f3ccd9"));

	// Composition property
	QCOMPARE(PQFT::hkdfSha384(salt, ikm, info, 42), okm);
}

void TestPQCrypto::x25519Rfc7748() {
	// RFC 7748 §5.2 one-iteration vector
	const QByteArray scalar =
		fromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
	const QByteArray ucoord =
		fromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
	QCOMPARE(PQFT::x25519SharedSecret(scalar, ucoord).toHex(),
			 QByteArray("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

	// RFC 7748 §6.1 Diffie-Hellman
	const QByteArray alicePriv =
		fromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	const QByteArray bobPub =
		fromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
	QCOMPARE(PQFT::x25519SharedSecret(alicePriv, bobPub).toHex(),
			 QByteArray("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
}

void TestPQCrypto::x25519RoundTrip() {
	QByteArray pubA, pubB;
	PQFT::SecureBytes secA, secB;
	QVERIFY(PQFT::x25519GenerateKeyPair(pubA, secA));
	QVERIFY(PQFT::x25519GenerateKeyPair(pubB, secB));
	QCOMPARE(pubA.size(), 32);
	QCOMPARE(secA.size(), 32);
	QVERIFY(!PQFT::isAllZero(pubA));

	const QByteArray ssA = PQFT::x25519SharedSecret(secA.toByteArray(), pubB);
	const QByteArray ssB = PQFT::x25519SharedSecret(secB.toByteArray(), pubA);
	QCOMPARE(ssA, ssB);
	QVERIFY(!ssA.isEmpty());
	QVERIFY(!PQFT::isAllZero(ssA));
}

void TestPQCrypto::x25519SmallOrderRejected() {
	// An all-zero peer point is of small order: OpenSSL refuses the
	// derivation; our wrapper surfaces that as an empty result. RFC 7748 §6.1.
	QByteArray pub;
	PQFT::SecureBytes sec;
	QVERIFY(PQFT::x25519GenerateKeyPair(pub, sec));
	QByteArray zero(32, '\0');
	QVERIFY(PQFT::x25519SharedSecret(sec.toByteArray(), zero).isEmpty());
}

void TestPQCrypto::aesGcmNistVector() {
	// NIST GCM test case 14 (AES-256, with AAD)
	const QByteArray key =
		fromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
	const QByteArray nonce = fromHex("cafebabefacedbaddecaf888");
	const QByteArray plaintext = fromHex(
		"d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
		"1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
	const QByteArray aad = fromHex("feedfacedeadbeeffeedfacedeadbeefabaddad2");

	QByteArray out;
	QVERIFY(PQFT::aesGcmEncrypt(out, key, nonce, plaintext, aad));
	QCOMPARE(out.size(), plaintext.size() + 16);
	QCOMPARE(out.toHex(),
			 QByteArray("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
						"8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
						"76fc6ece0f4e1768cddf8853bb2d551b"));

	QByteArray decrypted;
	QVERIFY(PQFT::aesGcmDecrypt(decrypted, key, nonce, out, aad));
	QCOMPARE(decrypted, plaintext);
}

void TestPQCrypto::aesGcmTamperFails() {
	const QByteArray key  = PQFT::randomBytes(32);
	const QByteArray nonce = PQFT::randomBytes(12);
	const QByteArray aad  = PQFT::randomBytes(24);
	const QByteArray plaintext = PQFT::randomBytes(1024);

	QByteArray out;
	QVERIFY(PQFT::aesGcmEncrypt(out, key, nonce, plaintext, aad));

	QByteArray tampered = out;
	tampered[5] = tampered[5] ^ 0x01;
	QByteArray decrypted;
	QVERIFY(!PQFT::aesGcmDecrypt(decrypted, key, nonce, tampered, aad));
	QVERIFY(decrypted.isEmpty());

	// Truncated tag
	QVERIFY(!PQFT::aesGcmDecrypt(decrypted, key, nonce, out.left(out.size() - 1), aad));
	// Wrong AAD
	QByteArray badAad = aad;
	badAad[0] = badAad[0] ^ 0x01;
	QVERIFY(!PQFT::aesGcmDecrypt(decrypted, key, nonce, out, badAad));
	// Wrong nonce
	QByteArray badNonce = nonce;
	badNonce[0] = badNonce[0] ^ 0x01;
	QVERIFY(!PQFT::aesGcmDecrypt(decrypted, key, badNonce, out, aad));
}

void TestPQCrypto::aesGcmEmptyPlaintext() {
	const QByteArray key   = PQFT::randomBytes(32);
	const QByteArray nonce = PQFT::randomBytes(12);
	const QByteArray aad   = PQFT::randomBytes(16);

	QByteArray out;
	QVERIFY(PQFT::aesGcmEncrypt(out, key, nonce, QByteArray(), aad));
	QCOMPARE(out.size(), 16); // just the tag

	QByteArray decrypted;
	QVERIFY(PQFT::aesGcmDecrypt(decrypted, key, nonce, out, aad));
	QVERIFY(decrypted.isEmpty());
}

void TestPQCrypto::mlkemRoundTrip() {
	PQFT::KemMLKEM768 kem;
	QVERIFY(kem.isValid());

	QByteArray pk, ct;
	PQFT::SecureBytes sk, ssEncap, ssDecap;
	QVERIFY(kem.keypair(pk, sk));
	QCOMPARE(pk.size(), PQFT::KemMLKEM768::PublicKeySize);
	QCOMPARE(sk.size(), PQFT::KemMLKEM768::SecretKeySize);

	QVERIFY(kem.encaps(ct, ssEncap, pk));
	QCOMPARE(ct.size(), PQFT::KemMLKEM768::CiphertextSize);
	QVERIFY(kem.decaps(ssDecap, ct, sk));
	QCOMPARE(ssEncap.size(), 32);
	QCOMPARE(ssDecap.size(), 32);
	QVERIFY(PQFT::constantTimeEquals(ssEncap.toByteArray(), ssDecap.toByteArray()));
}

void TestPQCrypto::mlkemBadCiphertext() {
	PQFT::KemMLKEM768 kem;
	QByteArray pk, ct;
	PQFT::SecureBytes sk, ssGood, ssBad;
	QVERIFY(kem.keypair(pk, sk));
	QVERIFY(kem.encaps(ct, ssGood, pk));

	QByteArray corrupted = ct;
	corrupted[10] = corrupted[10] ^ 0x01;
	// Implicit rejection: decapsulation "succeeds" but yields a different
	// (pseudorandom) secret — never an error branch on ciphertext content.
	QVERIFY(kem.decaps(ssBad, corrupted, sk));
	QVERIFY(!PQFT::constantTimeEquals(ssGood.toByteArray(), ssBad.toByteArray()));

	// Structurally invalid sizes are rejected outright
	QVERIFY(!kem.decaps(ssBad, ct.left(100), sk));
}

void TestPQCrypto::mldsaSignVerify() {
	PQFT::SigMLDSA65 sig;
	QVERIFY(sig.isValid());

	QByteArray pk;
	PQFT::SecureBytes sk;
	QVERIFY(sig.keypair(pk, sk));
	QCOMPARE(pk.size(), PQFT::SigMLDSA65::PublicKeySize);
	QCOMPARE(sk.size(), PQFT::SigMLDSA65::SecretKeySize);

	const QByteArray message = PQFT::randomBytes(256);
	const QByteArray ctx(PQFT::LabelHandshakeCtx);

	QByteArray signature;
	QVERIFY(sig.sign(signature, sk, message, ctx));
	QVERIFY(signature.size() > 0);
	QVERIFY(signature.size() <= PQFT::SigMLDSA65::MaxSignatureSize);
	QVERIFY(sig.verify(pk, message, signature, ctx));
}

void TestPQCrypto::mldsaTamperFails() {
	PQFT::SigMLDSA65 sig;
	QByteArray pk;
	PQFT::SecureBytes sk;
	QVERIFY(sig.keypair(pk, sk));

	const QByteArray message = QByteArray("attack at dawn");
	const QByteArray ctx(PQFT::LabelManifestCtx);

	QByteArray signature;
	QVERIFY(sig.sign(signature, sk, message, ctx));

	QByteArray badSig = signature;
	badSig[0] = badSig[0] ^ 0x01;
	QVERIFY(!sig.verify(pk, message, badSig, ctx));

	QVERIFY(!sig.verify(pk, QByteArray("attack at dusk"), signature, ctx));

	QVERIFY(!sig.verify(pk, message, signature.left(signature.size() - 1), ctx));
}

void TestPQCrypto::mldsaContextSeparated() {
	PQFT::SigMLDSA65 sig;
	QByteArray pk;
	PQFT::SecureBytes sk;
	QVERIFY(sig.keypair(pk, sk));

	const QByteArray message = PQFT::randomBytes(64);
	QByteArray sig1, sig2;
	QVERIFY(sig.sign(sig1, sk, message, QByteArray(PQFT::LabelHandshakeCtx)));
	QVERIFY(sig.sign(sig2, sk, message, QByteArray(PQFT::LabelManifestCtx)));

	// A signature made under one context does not verify under another
	QVERIFY(sig.verify(pk, message, sig1, QByteArray(PQFT::LabelHandshakeCtx)));
	QVERIFY(!sig.verify(pk, message, sig1, QByteArray(PQFT::LabelManifestCtx)));
	QVERIFY(!sig.verify(pk, message, sig2, QByteArray(PQFT::LabelHandshakeCtx)));
}

void TestPQCrypto::argon2Vector() {
	// Reference vector generated with the vendored upstream implementation
	// (independent execution path): argon2id, t=3, m=32 KiB, p=4,
	// password = 0x01*32, salt = 0x02*16. The wrapper enforces the
	// protocol MIN bounds, so the raw library call is used here.
	uint8_t pwd[32], salt[16], out[32];
	memset(pwd, 1, sizeof(pwd));
	memset(salt, 2, sizeof(salt));
	QCOMPARE(argon2id_hash_raw(3, 32, 4, pwd, sizeof(pwd), salt, sizeof(salt), out, sizeof(out)),
			 static_cast< int >(ARGON2_OK));
	QByteArray expected(32, Qt::Uninitialized);
	memcpy(expected.data(), out, 32);
	QCOMPARE(expected.toHex(),
			 QByteArray("03aab965c12001c9d7d0d2de33192c0494b684bb148196d73c1df1acaf6d0c2e"));
}

void TestPQCrypto::argon2Bounds() {
	PQFT::Argon2Params params; // defaults are within bounds
	QByteArray password("correct horse battery staple");
	QByteArray salt = PQFT::randomBytes(PQFT::Argon2SaltSize);
	PQFT::SecureBytes derived;
	QVERIFY(PQFT::argon2idDerive(derived, password, salt, params));
	QCOMPARE(derived.size(), 32);

	// Below MIN: fatal
	password = QByteArray("correct horse battery staple");
	params.memoryKiB = PQFT::Argon2MinMemoryKiB - 1;
	QVERIFY(!PQFT::argon2idDerive(derived, password, salt, params));

	// Above MAX: fatal
	password = QByteArray("correct horse battery staple");
	params.memoryKiB = PQFT::Argon2MaxMemoryKiB + 1;
	QVERIFY(!PQFT::argon2idDerive(derived, password, salt, params));

	// Time cost below MIN: fatal
	password = QByteArray("correct horse battery staple");
	params.memoryKiB = PQFT::Argon2DefaultMemoryKiB;
	params.timeCost   = PQFT::Argon2MinTimeCost - 1;
	QVERIFY(!PQFT::argon2idDerive(derived, password, salt, params));

	// Salt too small: fatal
	password = QByteArray("correct horse battery staple");
	params.timeCost = PQFT::Argon2DefaultTimeCost;
	QByteArray shortSalt(8, 'x');
	QVERIFY(!PQFT::argon2idDerive(derived, password, shortSalt, params));
}

void TestPQCrypto::cborEncodeVector() {
	// map(2) { 1: "a", 2: "b" } canonical encoding
	QVector< QPair< QCborValue, QCborValue > > entries;
	entries.append({ QCborValue(1), QCborValue(QString("a")) });
	entries.append({ QCborValue(2), QCborValue(QString("b")) });

	const QByteArray encoded = PQFT::encodeCanonicalMap(entries);
	QCOMPARE(encoded.toHex(), QByteArray("a2016161026162"));

	QCborValue decoded;
	QVERIFY(PQFT::decodeCanonical(encoded, decoded));
	QCOMPARE(decoded.type(), QCborValue::Map);

	// Keys are sorted by encoded bytes regardless of insertion order
	QVector< QPair< QCborValue, QCborValue > > reversed;
	reversed.append({ QCborValue(2), QCborValue(QString("b")) });
	reversed.append({ QCborValue(1), QCborValue(QString("a")) });
	QCOMPARE(PQFT::encodeCanonicalMap(reversed), encoded);
}

void TestPQCrypto::cborRejectsNonCanonical() {
	QCborValue value;

	// Trailing bytes
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a2016161026162") + '\x00', value));
	// Non-shortest integer encoding of 1 inside a map: 0x18 0x01
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a21801016161026162"), value));
	// Indefinite-length map
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("bf016161026162ff"), value));
	// Unsorted map keys
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a2026162016161"), value));
	// Null value: disallowed type
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a201f6026162"), value));
	// Truncated buffer
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a20161"), value));
	// Empty buffer
	QVERIFY(!PQFT::decodeCanonical(QByteArray(), value));
}

void TestPQCrypto::cborRejectsDuplicateKeys() {
	// map(2) { 1: "a", 1: "b" } — duplicate keys are non-canonical
	QCborValue value;
	QVERIFY(!PQFT::decodeCanonical(QByteArray::fromHex("a2016161016162"), value));

	QVector< QPair< QCborValue, QCborValue > > entries;
	entries.append({ QCborValue(1), QCborValue(QString("a")) });
	entries.append({ QCborValue(1), QCborValue(QString("b")) });
	QVERIFY(PQFT::encodeCanonicalMap(entries).isEmpty());
}

void TestPQCrypto::merkleVectors() {
	// Vectors computed with an independent implementation (Python hashlib)
	QVector< QByteArray > hashes = { PQFT::merkleChunkHash(QByteArray("alpha")),
									 PQFT::merkleChunkHash(QByteArray("beta")),
									 PQFT::merkleChunkHash(QByteArray("gamma")),
									 PQFT::merkleChunkHash(QByteArray("delta")) };

	QCOMPARE(hashes.at(0).toHex(),
			 QByteArray("9cc3c0f06e170b14d7c52a8cbfc31bf9e4cc491e2aa9b79a385bcffa62f6bc61"
						"9fcc95b5c1eb933dfad9c281c77208af"));
	QCOMPARE(PQFT::merkleRoot(hashes).toHex(),
			 QByteArray("449d12e6ea2d480d560cc4d049eed2cf208bff3e7ce48ec46abbd27cb6ff6c73"
						"e354590dc5a268ca890e2056021dc15b"));

	// Odd count duplicates the last node
	QVector< QByteArray > three = hashes.mid(0, 3);
	QCOMPARE(PQFT::merkleRoot(three).toHex(),
			 QByteArray("e0e230f08a2067a1de727d64ba929efbeda1857208ab0c55bc7edf53828aa6a9"
						"83d68ec4eeca2cc24afae7ed966d879a"));

	// Single chunk: root == leaf
	QVector< QByteArray > one = { hashes.at(0) };
	QCOMPARE(PQFT::merkleRoot(one), PQFT::merkleLeaf(hashes.at(0)));

	// Zero chunks: empty-input SHA-384
	QCOMPARE(PQFT::merkleRoot({}).toHex(),
			 QByteArray("38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
						"274edebfe76f65fbd51ad2f14898b95b"));

	// Order matters
	QVector< QByteArray > swapped = hashes;
	swapped.swapItemsAt(0, 1);
	QVERIFY(PQFT::merkleRoot(swapped) != PQFT::merkleRoot(hashes));
}

void TestPQCrypto::deviceHashCancellation() {
	class InterruptingBuffer : public QBuffer {
	public:
		int reads = 0;

	protected:
		qint64 readData(char *data, qint64 size) override {
			++reads;
			return QBuffer::readData(data, size);
		}
	} source;
	source.setData(QByteArray(8 * 16384, 'x'));
	QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
	QVERIFY(PQFT::merkleRootFromDevice(source, 16384, 8, [&]() { return source.reads >= 2; }).isEmpty());
	QCOMPARE(source.reads, 2);
	QVERIFY(source.seek(0));
	const QByteArray chunkHash = PQFT::merkleChunkHash(QByteArray(16384, 'x'));
	QCOMPARE(PQFT::merkleRootFromDevice(source, 16384, 8), PQFT::merkleRoot(QVector< QByteArray >(8, chunkHash)));
}

void TestPQCrypto::deviceHashRejectsWrongCount() {
	QBuffer source;
	source.setData(QByteArray(32768, 'x'));
	QVERIFY(source.open(QIODevice::ReadOnly));
	QVERIFY(PQFT::merkleRootFromDevice(source, 16384, 1).isEmpty());
	QVERIFY(source.seek(0));
	QVERIFY(PQFT::merkleRootFromDevice(source, 16384, 3).isEmpty());
	QVERIFY(source.seek(0));
	QVERIFY(PQFT::merkleRootFromDevice(source, 0, 2).isEmpty());
}

void TestPQCrypto::merkleCancellation_data() {
	QTest::addColumn< int >("cancelAt");
	QTest::newRow("before-input") << 1;
	QTest::newRow("validating-digests") << 5;
	QTest::newRow("building-leaves") << 14;
	QTest::newRow("building-parent-level") << 23;
}

void TestPQCrypto::merkleCancellation() {
	QFETCH(int, cancelAt);
	QVector< QByteArray > hashes(8, PQFT::merkleChunkHash(QByteArray("chunk")));
	int checks = 0;
	QVERIFY(PQFT::merkleRoot(hashes, [&]() { return ++checks == cancelAt; }).isEmpty());
	QCOMPARE(checks, cancelAt);
	QCOMPARE(PQFT::merkleRoot(hashes, []() { return false; }), PQFT::merkleRoot(hashes));
}

void TestPQCrypto::merkleRejectsBadLeaf() {
	QVector< QByteArray > bad = { PQFT::randomBytes(48), PQFT::randomBytes(47) };
	QVERIFY(PQFT::merkleRoot(bad).isEmpty());
}

QTEST_MAIN(TestPQCrypto)
#include "TestPQCrypto.moc"
