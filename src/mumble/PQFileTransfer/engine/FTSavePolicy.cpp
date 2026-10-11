// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "FTSavePolicy.h"
#include "Settings.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace PQFT {
bool isSafeAutomaticFileName(const QString &name) {
	if (name.isEmpty() || name.toUtf8().size() > MaxFileNameBytes || name.startsWith(QLatin1Char('.'))
		|| name.front().isSpace() || name.back().isSpace() || name.endsWith(QLatin1Char('.')))
		return false;
	for (const QChar ch : name) {
		if (QStringLiteral("/\\:<>\"|?*").contains(ch) || ch.category() == QChar::Other_Control
			|| ch.category() == QChar::Other_Format || ch.category() == QChar::Other_Surrogate
			|| ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph)
			return false;
	}
	// Windows device names stay reserved with an extension, including COM/LPT superscript digits.
	const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
	if (stem == QLatin1String("CON") || stem == QLatin1String("PRN") || stem == QLatin1String("AUX")
		|| stem == QLatin1String("NUL") || stem == QLatin1String("CLOCK$") || stem == QLatin1String("CONIN$")
		|| stem == QLatin1String("CONOUT$"))
		return false;
	if (stem.size() == 4 && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT")))) {
		const ushort digit = stem.at(3).unicode();
		if ((digit >= '0' && digit <= '9') || digit == 0x00b9 || digit == 0x00b2 || digit == 0x00b3)
			return false;
	}
	return true;
}

QString automaticSaveTarget(const Settings &settings, const FTTransferInfo &info, TrustState trust) {
	if (!settings.bFTAutoAcceptPinned || settings.qsFTDownloadDir.isEmpty()
		|| !QDir::isAbsolutePath(settings.qsFTDownloadDir) || !info.incoming
		|| info.state != FTTransferInfo::State::Ready || !info.error.isEmpty() || trust != TrustState::Verified
		|| info.peerFingerprint.size() != HashSize || info.peerName.isEmpty()
		|| !isSafeAutomaticFileName(info.fileName))
		return {};
	return QDir(settings.qsFTDownloadDir).filePath(info.fileName);
}

QString manualSaveDirectory(const Settings &settings) {
	if (!settings.qsFTManualSaveDir.isEmpty())
		return settings.qsFTManualSaveDir;
	if (!settings.qsFTDownloadDir.isEmpty())
		return settings.qsFTDownloadDir;
	return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

void rememberManualSaveDirectory(Settings &settings, const QString &target) {
	settings.qsFTManualSaveDir = QFileInfo(target).absolutePath();
}
} // namespace PQFT
