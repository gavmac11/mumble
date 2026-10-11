// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#ifndef MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTSAVEPOLICY_H_
#define MUMBLE_MUMBLE_PQFILETRANSFER_ENGINE_FTSAVEPOLICY_H_

#include "FileTransferEngine.h"
#include "PQFileTransfer/identity/PeerTrustStore.h"

struct Settings;

namespace PQFT {
/// Automatic saves use a conservative portable basename. Rejected files remain available for manual saving.
bool isSafeAutomaticFileName(const QString &name);

/// Returns an empty path unless this incoming file is ready, verified and eligible for automatic saving.
QString automaticSaveTarget(const Settings &settings, const FTTransferInfo &info, TrustState trust);

QString manualSaveDirectory(const Settings &settings);
/// Manual history never changes the configured automatic-save directory.
void rememberManualSaveDirectory(Settings &settings, const QString &target);
} // namespace PQFT

#endif
