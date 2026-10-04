// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CustomElements.h"
#include "Log.h"

#ifdef USE_CHAT_WEBM
#	include "ChatVideoDecoder.h"
#	include "ChatVideoPlayer.h"
#endif

#include <memory>

#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFragment>
#include <QtGui/QTextImageFormat>
#include <QtTest/QtTest>

// Global.h defines the global macro g and therefore has to be the final include.
#include "Global.h"

namespace {
// Tiny WebM test clips (128x96, 8 fps, 2 s). TINY_WEBM has a VP8 video and an
// Opus audio track, TINY_WEBM_SILENT is VP9 video without audio. Both are
// generated with the ffmpeg CLI (see below) followed by `base64 -w0`:
//
//   ffmpeg -f lavfi -i testsrc=size=128x96:rate=8:duration=2
//          -f lavfi -i sine=frequency=440:sample_rate=48000
//          -c:v libvpx -b:v 100k -c:a libopus -shortest tiny.webm
//
//   ffmpeg -f lavfi -i testsrc=size=128x96:rate=8:duration=2
//          -c:v libvpx-vp9 -b:v 100k -an tiny_silent.webm
const QByteArray TINY_WEBM         = QByteArray::fromBase64("");
const QByteArray TINY_WEBM_SILENT  = QByteArray::fromBase64("");

// EBML header magic followed by junk - a "WebM" that cannot be decoded
const QByteArray UNDECODABLE_WEBM = QByteArray::fromRawData("\x1A\x45\xDF\xA3\x96junk", 8);

class TestChatbarTextEdit : public ChatbarTextEdit {
public:
	using ChatbarTextEdit::canInsertFromMimeData;
	using ChatbarTextEdit::insertFromMimeData;
};

QString webmImageHtml(const QByteArray &webm) {
	return Log::videoToImg(webm, 0);
}

QUrl webmImageUrl(const QByteArray &webm) {
	const QString html = webmImageHtml(webm);
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

#ifdef USE_CHAT_WEBM
int chatVideoPlayerCount(const QObject &document) {
	return static_cast< int >(document.findChildren< ChatVideoPlayer * >().size());
}
#endif
} // namespace

class TestChatWebM : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void acceptsWebmFileDrop();
	void refusesOversizedWebm();
	void videoToImgRejectsGarbage();
	void videoToImgBudgetTooLarge();

#ifdef USE_CHAT_WEBM
	void placeholderForUndecodableWebm();
	void validationDocumentCreatesNoPlayers();
	void posterDoesNotAutoplay();
	void clickTogglesPlayback();
	void releasesPlayerOnClear();
	void retainsPlayerUntilLastReferenceIsRemoved();
	void releasesPlayerOnMaximumBlockEviction();
	void playingCapPausesOldest();
	void totalCapDestroysOldest();
	void scalesVideoWithChatWindow();
	void audioDecodesToPcm();
	void pacingMath();
#endif

private:
	std::unique_ptr< QTemporaryDir > m_configDir;
	std::unique_ptr< Global > m_global;
};

void TestChatWebM::initTestCase() {
	m_configDir             = std::make_unique< QTemporaryDir >();
	m_global                = std::make_unique< Global >(m_configDir->path());
	Global::g_global_struct = m_global.get();
}

void TestChatWebM::cleanupTestCase() {
	m_global.reset();
	Global::g_global_struct = nullptr;
	m_configDir.reset();
}

void TestChatWebM::acceptsWebmFileDrop() {
	QTemporaryDir dir;
	QFile file(dir.path() + "/test.webm");
	QVERIFY(file.open(QIODevice::WriteOnly));
	file.write(TINY_WEBM.isEmpty() ? UNDECODABLE_WEBM : TINY_WEBM);
	file.close();

	QMimeData mimeData;
	mimeData.setUrls({ QUrl::fromLocalFile(file.fileName()) });

	QVERIFY(!mimeData.hasImage());
	TestChatbarTextEdit chatbar;
	QVERIFY(chatbar.canInsertFromMimeData(&mimeData));
	QSignalSpy pastedImageSpy(&chatbar, &ChatbarTextEdit::pastedImage);
	chatbar.insertFromMimeData(&mimeData);
	QCOMPARE(pastedImageSpy.size(), 1);
	QVERIFY(pastedImageSpy.first().first().toString().contains(QLatin1String("data:video/webm;")));
}

void TestChatWebM::refusesOversizedWebm() {
	QTemporaryDir dir;
	QFile file(dir.path() + "/test.webm");
	QVERIFY(file.open(QIODevice::WriteOnly));
	file.write(UNDECODABLE_WEBM);
	file.write(QByteArray(10000, 'x'));
	file.close();

	QMimeData mimeData;
	mimeData.setUrls({ QUrl::fromLocalFile(file.fileName()) });

	const unsigned int previousImageLength = Global::get().uiImageLength;
	Global::get().uiImageLength            = 5000;

	TestChatbarTextEdit chatbar;
	QSignalSpy pastedImageSpy(&chatbar, &ChatbarTextEdit::pastedImage);
	chatbar.insertFromMimeData(&mimeData);
	QCOMPARE(pastedImageSpy.size(), 0);

	Global::get().uiImageLength = previousImageLength;
}

void TestChatWebM::videoToImgRejectsGarbage() {
	QVERIFY(Log::videoToImg(QByteArray("GIF89a-not-a-video-at-all"), 1000000).isEmpty());
	QVERIFY(Log::videoToImg(QByteArray(), 1000000).isEmpty());
}

void TestChatWebM::videoToImgBudgetTooLarge() {
	QVERIFY(Log::isWebM(UNDECODABLE_WEBM));
	QVERIFY(!Log::videoToImg(UNDECODABLE_WEBM, 1000000).isEmpty());
	QVERIFY(Log::videoToImg(UNDECODABLE_WEBM, 10).isEmpty());
}

#ifdef USE_CHAT_WEBM

void TestChatWebM::placeholderForUndecodableWebm() {
	LogDocument document(nullptr, true);
	const QUrl url = webmImageUrl(UNDECODABLE_WEBM);
	document.setHtml(webmImageHtml(UNDECODABLE_WEBM));

	QVERIFY(document.resource(QTextDocument::ImageResource, url).canConvert< QImage >());
	QCOMPARE(chatVideoPlayerCount(document), 0);
}

void TestChatWebM::validationDocumentCreatesNoPlayers() {
	// validHtml() parses into a non-animated LogDocument of this kind (it cannot
	// be called directly here - it needs a MainWindow for screen geometry).
	LogDocument validationDocument(nullptr, false);
	validationDocument.setHtml(webmImageHtml(UNDECODABLE_WEBM));

	QVERIFY(validationDocument.resource(QTextDocument::ImageResource, webmImageUrl(UNDECODABLE_WEBM))
				.canConvert< QImage >());
	QCOMPARE(chatVideoPlayerCount(validationDocument), 0);
}

void TestChatWebM::posterDoesNotAutoplay() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	const QUrl url = webmImageUrl(TINY_WEBM);
	document.setHtml(webmImageHtml(TINY_WEBM));

	const QVariant resource = document.resource(QTextDocument::ImageResource, url);
	QVERIFY(resource.canConvert< QImage >());
	const QImage initialFrame = resource.value< QImage >();
	QVERIFY(!initialFrame.isNull());
	QCOMPARE(initialFrame.size(), QSize(128, 96));
	QCOMPARE(chatVideoPlayerCount(document), 1);
	QCOMPARE(document.findChildren< ChatVideoPlayer * >().first()->isPlaying(), false);

	// Without a click, the shown frame must never change
	QTest::qWait(300);
	QCOMPARE(document.resource(QTextDocument::ImageResource, url).value< QImage >(), initialFrame);
}

void TestChatWebM::clickTogglesPlayback() {
	if (TINY_WEBM_SILENT.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogTextBrowser browser;
	browser.resize(400, 300);
	auto document = new LogDocument(&browser, true);
	browser.setDocument(document);
	document->setHtml(webmImageHtml(TINY_WEBM_SILENT));
	browser.resizeImagesToFit();

	const QUrl url = webmImageUrl(TINY_WEBM_SILENT);
	ChatVideoPlayer *player = document->findChildren< ChatVideoPlayer * >().first();
	QVERIFY(player);
	QCOMPARE(player->isPlaying(), false);

	// Click into the middle of the displayed video to start playback. The image
	// spans the whole first line of the document, so its rect derives from the
	// cursor at the document start (plus half the scaled image width).
	const QTextImageFormat format = firstImageFormat(*document);
	QVERIFY(format.isValid());
	QTextCursor cursor(document);
	const QRect lineRect = browser.cursorRect(cursor);
	const QPoint clickPos(lineRect.left() + static_cast< int >(format.width()) / 2, lineRect.center().y());
	QTest::mouseClick(browser.viewport(), Qt::LeftButton, {}, clickPos);
	QCOMPARE(player->isPlaying(), true);

	// Frames must advance while playing
	QTest::qWait(500);
	QCOMPARE(document->findChildren< ChatVideoPlayer * >().first()->isPlaying(), true);

	// Click again to pause: playback stops and the shown frame freezes
	QTest::mouseClick(browser.viewport(), Qt::LeftButton, {}, clickPos);
	QCOMPARE(player->isPlaying(), false);
	const QImage pausedFrame = document->resource(QTextDocument::ImageResource, url).value< QImage >();
	QTest::qWait(200);
	QCOMPARE(document->resource(QTextDocument::ImageResource, url).value< QImage >(), pausedFrame);
}

void TestChatWebM::releasesPlayerOnClear() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	document.setHtml(webmImageHtml(TINY_WEBM));
	QCOMPARE(chatVideoPlayerCount(document), 1);

	document.clear();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(chatVideoPlayerCount(document), 0);
}

void TestChatWebM::retainsPlayerUntilLastReferenceIsRemoved() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	const QString html = webmImageHtml(TINY_WEBM);
	document.setHtml(html + html);
	QCOMPARE(chatVideoPlayerCount(document), 1);

	document.setHtml(html);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(chatVideoPlayerCount(document), 1);

	document.clear();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(chatVideoPlayerCount(document), 0);
}

void TestChatWebM::releasesPlayerOnMaximumBlockEviction() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	document.setMaximumBlockCount(1);
	document.setHtml(webmImageHtml(TINY_WEBM));
	QCOMPARE(chatVideoPlayerCount(document), 1);

	QTextCursor cursor(&document);
	cursor.movePosition(QTextCursor::End);
	cursor.insertBlock();
	cursor.insertText(QLatin1String("evict image block"));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCOMPARE(chatVideoPlayerCount(document), 0);
}

namespace {
/// Returns a copy of the silent fixture padded with trailing zero bytes. The
/// padding makes the data (and thereby its data-URL) distinct without breaking
/// the EBML magic or the decodability of the leading frames, so several
/// independent players can be created from a single fixture.
QByteArray distinctWebM(int index) {
	QByteArray data = TINY_WEBM_SILENT;
	data.append(QByteArray((index + 1) * 64, '\0'));
	return data;
}
} // namespace

void TestChatWebM::playingCapPausesOldest() {
	if (TINY_WEBM_SILENT.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	QList< ChatVideoPlayer * > players;
	for (int i = 0; i <= LogDocument::MAX_PLAYING_CHAT_VIDEOS; ++i) {
		QTextCursor cursor(&document);
		cursor.movePosition(QTextCursor::End);
		cursor.insertHtml(webmImageHtml(distinctWebM(i)));
		players.append(document.findChildren< ChatVideoPlayer * >().last());
	}
	QCOMPARE(players.size(), LogDocument::MAX_PLAYING_CHAT_VIDEOS + 1);

	for (ChatVideoPlayer *player : players) {
		player->toggle();
		QTest::qWait(10);
	}

	// The oldest playing video has been paused to make room for the newest one
	QCOMPARE(players.first()->isPlaying(), false);
	for (int i = 1; i < players.size(); ++i) {
		QCOMPARE(players.at(i)->isPlaying(), true);
	}
}

void TestChatWebM::totalCapDestroysOldest() {
	if (TINY_WEBM_SILENT.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	LogDocument document(nullptr, true);
	for (int i = 0; i <= LogDocument::MAX_CHAT_VIDEOS; ++i) {
		QTextCursor cursor(&document);
		cursor.movePosition(QTextCursor::End);
		cursor.insertHtml(webmImageHtml(distinctWebM(i)));
	}
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	// The oldest player was destroyed to make room for the newest one
	QCOMPARE(document.findChildren< ChatVideoPlayer * >().size(), LogDocument::MAX_CHAT_VIDEOS);
}

void TestChatWebM::scalesVideoWithChatWindow() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	QString html = webmImageHtml(TINY_WEBM);
	html.replace(QLatin1String(" />"), QLatin1String(" width=\"600\" height=\"300\" />"));

	LogTextBrowser browser;
	browser.resize(240, 160);
	auto document = new LogDocument(&browser, true);
	browser.setDocument(document);
	document->setHtml(html);
	browser.resizeImagesToFit();

	const QTextImageFormat format = firstImageFormat(*document);
	QVERIFY(format.isValid());
	QVERIFY(format.width() <= browser.viewport()->width());
	QVERIFY(format.height() <= browser.viewport()->height());
	QCOMPARE(document->findChildren< ChatVideoPlayer * >().size(), 1);
}

void TestChatWebM::audioDecodesToPcm() {
	if (TINY_WEBM.isEmpty() || !ChatVideoDecoder::isSupported()) {
		QSKIP("WebM test fixture not available");
	}

	ChatVideoDecoder decoder;
	QVERIFY(decoder.open(TINY_WEBM, true));
	if (!decoder.decodingAudio()) {
		QSKIP("No audio decoder available in this FFmpeg build");
	}

	bool sawAudio    = false;
	qint64 lastPtsMs = -1;
	ChatVideoDecoder::VideoFrame videoFrame;
	ChatVideoDecoder::AudioChunk audioChunk;
	while (decoder.step(videoFrame, audioChunk)) {
		if (audioChunk.isValid()) {
			sawAudio = true;
			QVERIFY(audioChunk.ptsMs >= lastPtsMs);
			lastPtsMs = audioChunk.ptsMs;
			// S16LE stereo samples: the byte count has to be a multiple of 4
			QCOMPARE(audioChunk.pcm.size() % 4, 0);
		}
	}
	QVERIFY(sawAudio);
	QVERIFY(lastPtsMs >= 0);
}

void TestChatWebM::pacingMath() {
	// Frame due times are the stream pts offset by the loop accumulator
	QCOMPARE(ChatVideoPlayer::frameDueMs(100, 1000), 1100);

	// A frame may be presented at most 5 ms early
	QVERIFY(ChatVideoPlayer::shouldPresentFrame(1105, 1100));
	QVERIFY(!ChatVideoPlayer::shouldPresentFrame(1106, 1100));

	// A frame more than 40 ms late is dropped instead of presented
	QVERIFY(ChatVideoPlayer::shouldDropFrame(1059, 1100));
	QVERIFY(!ChatVideoPlayer::shouldDropFrame(1060, 1100));
	QVERIFY(!ChatVideoPlayer::shouldDropFrame(1100, 1100));

	// The play overlay keeps the image dimensions
	QImage plain(64, 48, QImage::Format_RGBA8888);
	plain.fill(Qt::red);
	const QImage overlay = ChatVideoPlayer::paintPlayOverlay(plain);
	QCOMPARE(overlay.size(), plain.size());
	QVERIFY(overlay != plain);
}

#endif // USE_CHAT_WEBM

QTEST_MAIN(TestChatWebM)
#include "TestChatWebM.moc"
