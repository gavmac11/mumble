// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.
//
// FileTransferManager is the front door of the chat file-transfer feature.
// It lives on the GUI thread, owns the worker thread running
// FileTransferEngine, the identity and the TOFU trust store, and forwards
// between the ServerHandler thread, the worker and the UI. See PROTOCOL.md.

#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_

#include "PQFileTransfer/engine/FileTransferEngine.h"
#include "PQFileTransfer/identity/FTIdentity.h"
#include "PQFileTransfer/identity/PeerTrustStore.h"

#include <Mumble.pb.h>

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QThread;

using PQFT::FileTransferIdentity;
using PQFT::PeerTrustStore;

class FileTransferManager : public QObject {
	Q_OBJECT
public:
	explicit FileTransferManager(QObject *parent = nullptr);
	~FileTransferManager() override;

	/// ServerHandler thread: route a control record into the worker.
	void handleControlMessage(const MumbleProto::FileTransferControl &msg);
	/// ServerHandler thread: route a chunk into the worker.
	void handleDataMessage(const MumbleProto::FileData &msg);

	// ---- GUI-thread API ------------------------------------------------

	FileTransferIdentity *identity() const;
	PeerTrustStore *trustStore() const;

	/// True when the local identity exists and is unlocked (usable).
	bool hasUsableIdentity() const;

	/// Hand the (unlocked) identity's public key to the engine; call after
	/// creating or unlocking the identity.
	void pushIdentityToEngine();

	/// Queue a file for sending to `recipients` (sessions with pinned
	/// identities). Returns false when prerequisites are missing.
	bool startSend(const QString &filePath, const QString &mimeType, bool passwordMode,
				   const QByteArray &password, const QList< unsigned int > &recipients);

	void abortTransfer(const QByteArray &transferId);
	void saveTransferAs(const QByteArray &transferId, const QString &targetPath);
	void providePassword(const QByteArray &transferId, const QByteArray &password);

	/// Called by the safety-number dialog: pin the (possibly new) fingerprint
	/// and continue (verified) or drop (declined) the pending handshake.
	void resolveFirstContact(unsigned int peerSession, bool verified);

	/// Pin a peer's fingerprint (first use), optionally marking it verified.
	/// Returns false when the peer cannot be resolved. GUI thread only.
	bool pinPeer(unsigned int peerSession, bool verified);

	/// TOFU state of a channel member for the pre-send dialog (live DB read).
	PQFT::TrustState trustStateFor(unsigned int peerSession, QByteArray &pinnedFingerprint);
	/// Safety number of a peer against our identity (for verification UI).
	QString safetyNumberFor(unsigned int peerSession);

	/// Drop every in-flight transfer (disconnects, server changes).
	void disconnectCleanup();

	/// Rebuild the session->fingerprint pin cache from the user list.
	void refreshPinCache();

	/// Apply current settings to the engine.
	void applyEngineConfig();

signals:
	/// Queued from the worker whenever a transfer's state changes; also
	/// raised directly for UI-initiated flows.
	void transferUpdated(const PQFT::FTTransferInfo &info);
	/// A peer contacted us for the first time: the mandatory safety-number
	/// verification must happen before the handshake continues.
	void firstContact(unsigned int peerSession, const QByteArray &peerFingerprint,
					  const QString &safetyNumber);
	/// A pinned peer presented a different identity: hard-blocked.
	void peerBlocked(unsigned int peerSession, const QString &peerName);
	/// An incoming password-mode transfer needs its password.
	void passwordRequired(const QByteArray &transferId, const QString &fileName);

private:
	void setupEngineTransports();
	QByteArray serverDigest() const;

	std::unique_ptr< QThread > m_workerThread;
	PQFT::FileTransferEngine *m_engine = nullptr;   // lives on the worker thread

	std::unique_ptr< FileTransferIdentity > m_identity;
	std::unique_ptr< PeerTrustStore > m_trustStore;

	/// Session -> pinned fingerprint cache, guarded for the worker's lookups.
	QMutex m_pinCacheMutex;
	QHash< unsigned int, QByteArray > m_pinCache;

	/// Receiver-side first contacts awaiting the user's decision:
	/// session -> (presented fingerprint, safety number). In memory only — a
	/// declined peer must leave NO pin behind, so nothing goes to the trust
	/// store until the safety-number dialog is accepted. GUI thread only.
	QHash< unsigned int, QPair< QByteArray, QString > > m_pendingFirstContact;
};

#endif // MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FILETRANSFERMANAGER_H_
