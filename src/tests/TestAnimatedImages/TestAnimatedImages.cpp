// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CustomElements.h"
#include "Log.h"

#include <memory>

#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtGui/QMovie>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFragment>
#include <QtGui/QTextImageFormat>
#include <QtTest/QtTest>

// Global.h defines the global macro g and therefore has to be the final include.
#include "Global.h"

namespace {
const QByteArray ANIMATED_GIF = QByteArray::fromBase64(
	"R0lGODlhAgACAPAAAP8AAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAAAAAACwAAAAAAgACAAACAoRRACH5BAAAAAAALAAA"
	"AAACAAIAgAAA/wAAAAIChFEAOw==");

class TestChatbarTextEdit : public ChatbarTextEdit {
public:
	using ChatbarTextEdit::canInsertFromMimeData;
	using ChatbarTextEdit::insertFromMimeData;
};

QString animatedImageHtml() {
	return Log::imageToImg(QByteArray("gif"), ANIMATED_GIF);
}

QUrl animatedImageUrl() {
	const QString html = animatedImageHtml();
	return QUrl(html.mid(10, html.size() - 14));
}

QTextImageFormat firstImageFormat(const QTextDocument &document) {
	for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
		for (auto it = block.begin(); !it.atEnd(); ++it) {
			const QTextFragment fragment = it.fragment();
			if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
				return fragment.charFormat().toImageFormat();
			}
		}
	}

	return QTextImageFormat();
}

void loadAnimatedResource(LogDocument &document, const QUrl &url) {
	QVERIFY(document.resource(QTextDocument::ImageResource, url).canConvert< QImage >());
	QCOMPARE(document.findChildren< QMovie * >().size(), 1);
}
} // namespace

class TestAnimatedImages : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();
	void acceptsRawGifMimeData();
	void pastesRawGifMimeDataWithStaticFallback();
	void rawGifHasStaticFallback();
	void releasesAnimationOnClear();
	void retainsAnimationUntilLastReferenceIsRemoved();
	void releasesAnimationOnMaximumBlockEviction();
	void scalesStaticImageWithChatWindow();
	void scalesAnimatedGifWithoutStoppingAnimation();

private:
	std::unique_ptr< QTemporaryDir > m_configDir;
	std::unique_ptr< Global > m_global;
};

void TestAnimatedImages::initTestCase() {
	m_configDir             = std::make_unique< QTemporaryDir >();
	m_global                = std::make_unique< Global >(m_configDir->path());
	Global::g_global_struct = m_global.get();
}

void TestAnimatedImages::cleanupTestCase() {
	m_global.reset();
	Global::g_global_struct = nullptr;
	m_configDir.reset();
}

void TestAnimatedImages::acceptsRawGifMimeData() {
	QMimeData mimeData;
	mimeData.setData(QLatin1String("image/gif"), ANIMATED_GIF);

	QVERIFY(!mimeData.hasImage());
	TestChatbarTextEdit chatbar;
	QVERIFY(chatbar.canInsertFromMimeData(&mimeData));
	QSignalSpy pastedImageSpy(&chatbar, &ChatbarTextEdit::pastedImage);
	chatbar.insertFromMimeData(&mimeData);
	QCOMPARE(pastedImageSpy.size(), 1);
	QVERIFY(pastedImageSpy.first().first().toString().contains(QLatin1String("data:image/gif;")));
}

void TestAnimatedImages::pastesRawGifMimeDataWithStaticFallback() {
	QByteArray oversizedGif = ANIMATED_GIF;
	oversizedGif.append(10000, 'x');
	QMimeData mimeData;
	mimeData.setData(QLatin1String("image/gif"), oversizedGif);

	const unsigned int previousImageLength = Global::get().uiImageLength;
	Global::get().uiImageLength            = 5000;
	TestChatbarTextEdit chatbar;
	QSignalSpy pastedImageSpy(&chatbar, &ChatbarTextEdit::pastedImage);
	chatbar.insertFromMimeData(&mimeData);
	QCOMPARE(pastedImageSpy.size(), 1);
	QVERIFY(pastedImageSpy.first().first().toString().contains(QLatin1String("data:image/jpeg;"), Qt::CaseInsensitive));
	Global::get().uiImageLength = previousImageLength;
}

void TestAnimatedImages::rawGifHasStaticFallback() {
	QByteArray oversizedGif = ANIMATED_GIF;
	oversizedGif.append(10000, 'x');
	const QImage firstFrame = QImage::fromData(oversizedGif, "GIF");
	QVERIFY(!firstFrame.isNull());

	const QString html = Log::imageToImg(oversizedGif, firstFrame, 5000);
	QVERIFY(!html.isEmpty());
	QVERIFY(html.startsWith(QLatin1String("<img src=\"data:image/jpeg;"), Qt::CaseInsensitive));
}

void TestAnimatedImages::releasesAnimationOnClear() {
	LogDocument document(nullptr, true);
	const QUrl url = animatedImageUrl();
	document.setHtml(animatedImageHtml());
	loadAnimatedResource(document, url);

	document.clear();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(document.findChildren< QMovie * >().size(), 0);
}

void TestAnimatedImages::retainsAnimationUntilLastReferenceIsRemoved() {
	LogDocument document(nullptr, true);
	const QUrl url      = animatedImageUrl();
	const QString image = animatedImageHtml();
	document.setHtml(image + image);
	loadAnimatedResource(document, url);

	document.setHtml(image);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(document.findChildren< QMovie * >().size(), 1);

	document.clear();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(document.findChildren< QMovie * >().size(), 0);
}

void TestAnimatedImages::releasesAnimationOnMaximumBlockEviction() {
	LogDocument document(nullptr, true);
	document.setMaximumBlockCount(1);
	const QUrl url = animatedImageUrl();
	document.setHtml(animatedImageHtml());
	loadAnimatedResource(document, url);

	QTextCursor cursor(&document);
	cursor.movePosition(QTextCursor::End);
	cursor.insertBlock();
	cursor.insertText(QLatin1String("evict image block"));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(document.findChildren< QMovie * >().size(), 0);
}

void TestAnimatedImages::scalesStaticImageWithChatWindow() {
	QImage image(600, 300, QImage::Format_RGB32);
	image.fill(Qt::blue);

	LogTextBrowser browser;
	browser.resize(240, 160);
	browser.setHtml(Log::imageToImg(image));
	browser.resizeImagesToFit();

	QTextImageFormat format = firstImageFormat(*browser.document());
	QVERIFY(format.isValid());
	QVERIFY(format.width() <= browser.viewport()->width());
	QVERIFY(format.height() <= browser.viewport()->height());
	QCOMPARE(format.width() / format.height(), 2.0);
	const QImage storedImage =
		browser.document()->resource(QTextDocument::ImageResource, QUrl(format.name())).value< QImage >();
	QCOMPARE(storedImage.size(), image.size());

	browser.resize(800, 500);
	browser.resizeImagesToFit();
	format = firstImageFormat(*browser.document());
	QCOMPARE(format.width(), 600.0);
	QCOMPARE(format.height(), 300.0);
}

void TestAnimatedImages::scalesAnimatedGifWithoutStoppingAnimation() {
	QString html = animatedImageHtml();
	html.replace(QLatin1String(" />"), QLatin1String(" width=\"600\" height=\"300\" />"));

	LogTextBrowser browser;
	browser.resize(240, 160);
	auto document = new LogDocument(&browser, true);
	browser.setDocument(document);
	browser.setHtml(html);
	browser.resizeImagesToFit();

	const QTextImageFormat format = firstImageFormat(*document);
	QVERIFY(format.isValid());
	QVERIFY(format.width() <= browser.viewport()->width());
	QVERIFY(format.height() <= browser.viewport()->height());
	QCOMPARE(format.width() / format.height(), 2.0);
	QCOMPARE(document->findChildren< QMovie * >().size(), 1);
}

QTEST_MAIN(TestAnimatedImages)
#include "TestAnimatedImages.moc"
