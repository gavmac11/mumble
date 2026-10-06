// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FTCardRender.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace PQFT {

namespace {
constexpr int CardHeight		 = 84;
constexpr int Margin			 = 10;
constexpr int IconSize			 = 36;
constexpr int ProgressBarHeight = 6;

QString humanSize(quint64 bytes) {
	constexpr double KiB = 1024.0, MiB = 1024.0 * KiB, GiB = 1024.0 * MiB;
	const double value = static_cast< double >(bytes);
	if (value >= GiB) {
		return QString::number(value / GiB, 'f', 2) + QStringLiteral(" GiB");
	} else if (value >= MiB) {
		return QString::number(value / MiB, 'f', 1) + QStringLiteral(" MiB");
	} else if (value >= KiB) {
		return QString::number(value / KiB, 'f', 1) + QStringLiteral(" KiB");
	}
	return QString::number(bytes) + QStringLiteral(" B");
}

QString stateLabel(const FTTransferInfo &info) {
	switch (info.state) {
		case FTTransferInfo::State::Handshaking:
			return FileTransferEngine::tr("Connecting…");
		case FTTransferInfo::State::VerifyingIdentity:
			return FileTransferEngine::tr("Verify sender identity");
		case FTTransferInfo::State::WaitingPassword:
			return FileTransferEngine::tr("Password required — click to enter");
		case FTTransferInfo::State::Transferring:
			return FileTransferEngine::tr("Transferring…");
		case FTTransferInfo::State::Verifying:
			return FileTransferEngine::tr("Verifying…");
		case FTTransferInfo::State::Ready:
			return FileTransferEngine::tr("Click to save");
		case FTTransferInfo::State::Saved:
			return FileTransferEngine::tr("Saved");
		case FTTransferInfo::State::Failed:
			return info.error.isEmpty() ? FileTransferEngine::tr("Failed") : info.error;
		case FTTransferInfo::State::Aborted:
			return FileTransferEngine::tr("Cancelled");
	}
	return QString();
}
} // namespace

QString fileCardToHtml(const QByteArray &transferId) {
	return QStringLiteral("<br /><img src=\"data:application/mumble-file;base64,%1\" alt=\"%2\" />")
		.arg(QString::fromLatin1(transferId.toBase64()),
			 QString::fromLatin1(transferId.toHex()));
}

QByteArray fileCardTransferId(const QUrl &url, bool &ok) {
	// Decodes exactly like Log::imageDataFromDataUrl
	ok = false;
	if (url.scheme() != QLatin1String("data") || !url.host().isEmpty()) {
		return QByteArray();
	}
	QByteArray data = QByteArray::fromPercentEncoding(
		url.toString(QUrl::FullyEncoded | QUrl::RemoveScheme).toLatin1());
	const int comma = static_cast< int >(data.indexOf(','));
	if (comma == -1) {
		return QByteArray();
	}
	QByteArray payload = data.mid(comma + 1);
	data.truncate(comma);
	data = data.trimmed();
	if (data.endsWith(";base64")) {
		payload = QByteArray::fromBase64(payload);
		data.chop(7);
	}
	if (payload.size() == TransferIdSize) {
		ok = true;
	}
	return payload;
}

QImage renderFileCard(const FTTransferInfo &info, int width, qreal dpr) {
	QImage image(qRound(width * dpr), qRound(CardHeight * dpr), QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(dpr);
	image.fill(Qt::transparent);

	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);

	// Card background
	QPainterPath rounded;
	rounded.addRoundedRect(QRectF(0.5, 0.5, width - 1.0, CardHeight - 1.0), 8, 8);
	painter.fillPath(rounded, QColor(0x2a, 0x2d, 0x33));
	painter.setPen(QPen(QColor(0x4a, 0x4f, 0x59), 1.0));
	painter.drawPath(rounded);

	// File glyph: a page with a folded corner
	QRectF pageRect(Margin, (CardHeight - IconSize) / 2.0, IconSize * 0.78, IconSize);
	QPainterPath page;
	page.moveTo(pageRect.topLeft());
	page.lineTo(pageRect.topRight() - QPointF(10, 0));
	page.lineTo(pageRect.bottomRight());
	page.lineTo(pageRect.bottomLeft());
	page.closeSubpath();
	QPainterPath fold;
	fold.moveTo(pageRect.topRight() - QPointF(10, 0));
	fold.lineTo(pageRect.topRight() + QPointF(0, 10));
	fold.lineTo(pageRect.topRight() - QPointF(10, 10));
	fold.closeSubpath();
	painter.fillPath(page, QColor(0xd8, 0xdc, 0xe3));
	painter.fillPath(fold, QColor(0xa8, 0xad, 0xb5));

	// Text block
	const int textX = Margin + IconSize + 8;
	const int textWidth = width - textX - Margin - 8;

	QFont nameFont = painter.font();
	nameFont.setBold(true);
	nameFont.setPointSizeF(nameFont.pointSizeF() * 1.05);
	painter.setFont(nameFont);
	const QString elidedName = QFontMetrics(nameFont).elidedText(
		info.fileName.isEmpty() ? QStringLiteral("(unknown file)") : info.fileName, Qt::ElideMiddle,
		textWidth - (info.passwordMode ? 30 : 0));
	painter.setPen(Qt::white);
	painter.drawText(QRect(textX, Margin + 2, textWidth, 20), Qt::AlignLeft | Qt::AlignVCenter,
					 elidedName);

	painter.setFont(QFont());
	painter.setPen(QColor(0xb8, 0xbd, 0xc6));
	const QString meta = QString("%1 · %2%3")
							 .arg(info.incoming ? FileTransferEngine::tr("received file")
												: FileTransferEngine::tr("sent file"))
							 .arg(humanSize(info.fileSize))
							 .arg(info.passwordMode
									  ? QStringLiteral(" · ") + FileTransferEngine::tr("encrypted")
									  : QString());
	painter.drawText(QRect(textX, Margin + 24, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
					 meta);

	// Progress bar or state line
	const int barY = CardHeight - Margin - 18;
	if (info.state == FTTransferInfo::State::Transferring && info.fileSize > 0) {
		const int barWidth = textWidth;
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0x3a, 0x3f, 0x47));
		painter.drawRoundedRect(QRect(textX, barY, barWidth, ProgressBarHeight), 3, 3);
		const double fraction = info.fileSize > 0
			? qBound(0.0, static_cast< double >(info.bytesDone) / static_cast< double >(info.fileSize), 1.0)
			: 0.0;
		painter.setBrush(QColor(0x4f, 0xa3, 0xe8));
		painter.drawRoundedRect(QRect(textX, barY, qRound(barWidth * fraction), ProgressBarHeight), 3, 3);
	} else {
		painter.setPen(info.state == FTTransferInfo::State::Failed ? QColor(0xe8, 0x74, 0x6f)
					 : info.state == FTTransferInfo::State::Ready
						 ? QColor(0x7d, 0xd4, 0x8a)
						 : QColor(0x9a, 0xa2, 0xae));
		painter.drawText(QRect(textX, barY - 6, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
						 stateLabel(info));
	}

	// Lock badge for password mode
	if (info.passwordMode) {
		const int badgeSize = 20;
		const QRectF badgeRect(width - Margin - badgeSize - 6, Margin, badgeSize, badgeSize);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0xe8, 0xb4, 0x4f));
		painter.drawRoundedRect(badgeRect, 4, 4);
		const qreal shackleWidth = 8;
		const QRectF shackleRect(badgeRect.center().x() - shackleWidth / 2, badgeRect.y() - 1,
								 shackleWidth, badgeSize * 0.75);
		QPainterPath shackle;
		shackle.arcMoveTo(shackleRect, 180);
		shackle.arcTo(shackleRect, 180, -180);
		painter.setPen(QPen(QColor(0x2a, 0x2d, 0x33), 2));
		painter.setBrush(Qt::NoBrush);
		painter.drawPath(shackle);
		// keyhole dot
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0x2a, 0x2d, 0x33));
		painter.drawEllipse(badgeRect.center() + QPointF(0, 3), 1.6, 1.6);
	}

	return image;
}

} // namespace PQFT
