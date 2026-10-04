// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatVideoPlayer.h"

#ifdef USE_CHAT_WEBM

#include "ChatVideoDecoder.h"

#include <QtCore/QThread>
#include <QtGui/QPainter>
#include <QtGui/QPolygonF>

#ifdef USE_CHAT_WEBM_AUDIO
#include <QtMultimedia/QAudioSink>
#include <QtCore/QIODevice>
#endif

ChatVideoPlayer *ChatVideoPlayer::create(const QUrl &url, const QByteArray &videoData, QImage poster, QObject *parent) {
	if (poster.isNull() || !ChatVideoDecoder::isSupported())
		return nullptr;

	return new ChatVideoPlayer(url, videoData, std::move(poster), parent);
}

ChatVideoPlayer::ChatVideoPlayer(const QUrl &url, const QByteArray &videoData, QImage poster, QObject *parent)
	: QObject(parent), m_url(url), m_videoData(videoData), m_poster(std::move(poster)),
	  m_displayImage(paintPlayOverlay(m_poster)) {
}

ChatVideoPlayer::~ChatVideoPlayer() {
#ifdef USE_CHAT_WEBM_AUDIO
	if (m_sink) {
		m_sink->stop();
		delete m_sink;
		m_sink   = nullptr;
		m_audioIo = nullptr;
	}
#endif

	if (m_thread) {
		// Make sure the worker stops decoding before it is destroyed.
		QMetaObject::invokeMethod(m_decoder, "shutdown", Qt::BlockingQueuedConnection);
		m_thread->quit();
		m_thread->wait(2000);
	}
}

QImage ChatVideoPlayer::paintPlayOverlay(const QImage &image) {
	if (image.isNull())
		return image;

	QImage result = image;
	if (result.format() != QImage::Format_ARGB32 && result.format() != QImage::Format_RGBA8888) {
		result = result.convertToFormat(QImage::Format_RGBA8888);
	}

	QPainter painter(&result);
	painter.setRenderHint(QPainter::Antialiasing);

	const qreal radius = qMin(result.width(), result.height()) * 0.2;
	const QPointF center(result.width() / 2.0, result.height() / 2.0);

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 160));
	painter.drawEllipse(center, radius, radius);

	painter.setBrush(Qt::white);
	QPolygonF playButton;
	playButton << QPointF(center.x() - radius * 0.35, center.y() - radius * 0.5)
			   << QPointF(center.x() - radius * 0.35, center.y() + radius * 0.5)
			   << QPointF(center.x() + radius * 0.55, center.y());
	painter.drawPolygon(playButton);

	return result;
}

bool ChatVideoPlayer::ensureWorker() {
	if (m_thread)
		return true;

#ifdef USE_CHAT_WEBM_AUDIO
	const bool decodeAudio = ensureAudioSink();
#else
	const bool decodeAudio = false;
#endif

	// The decoder is opened on the GUI thread and only then handed over to its
	// worker thread - afterwards it is exclusively used from that thread.
	m_decoder = new ChatVideoDecoder();
	if (!m_decoder->open(m_videoData, decodeAudio)) {
		delete m_decoder;
		m_decoder = nullptr;
#ifdef USE_CHAT_WEBM_AUDIO
		if (m_sink) {
			m_sink->stop();
			delete m_sink;
			m_sink   = nullptr;
			m_audioIo = nullptr;
		}
#endif
		return false;
	}

	// Cache the duration while the decoder is still owned by this thread - after
	// the move it may only be touched from its worker thread.
	m_durationMs = m_decoder->durationMs();

	m_thread = new QThread(this);
	m_decoder->moveToThread(m_thread);
	connect(m_thread, &QThread::finished, m_decoder, &QObject::deleteLater);

	connect(m_decoder, &ChatVideoDecoder::videoFrameDecoded, this, &ChatVideoPlayer::onDecoderVideoFrame);
	connect(m_decoder, &ChatVideoDecoder::audioChunkDecoded, this, &ChatVideoPlayer::onDecoderAudioChunk);
	connect(m_decoder, &ChatVideoDecoder::finished, this, &ChatVideoPlayer::onDecoderFinished);
	connect(m_decoder, &ChatVideoDecoder::error, this, &ChatVideoPlayer::onDecoderError);

	m_thread->start();
	return true;
}

#ifdef USE_CHAT_WEBM_AUDIO
bool ChatVideoPlayer::ensureAudioSink() {
	if (m_sink)
		return m_audioIo != nullptr;

	QAudioFormat format;
	format.setSampleFormat(QAudioFormat::Int16);
	format.setSampleRate(48000);
	format.setChannelCount(2);

	QAudioSink *sink = new QAudioSink(format, this);
	QIODevice *io    = sink->start();
	if (!io || sink->error() == QAudio::FatalError) {
		qWarning("ChatVideoPlayer: unable to open an audio output - playing without sound");
		delete sink;
		return false;
	}

	connect(sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State state) {
		if (state == QAudio::AudioError && m_state == Playing) {
			qWarning("ChatVideoPlayer: audio output error - continuing without sound");
			// Fall back to the wall clock, continuing from the current position.
			m_loopAccumMs = currentClockMs();
			m_audioBuffer.clear();
			m_audioIo     = nullptr;
			m_wallClock.start();
			m_tsAtIterationStart = 0;
			// Detach the broken sink (stopped and deleted at destruction).
			sink->disconnect(this);
		}
	});

	m_sink   = sink;
	m_audioIo = io;
	return true;
}
#endif

qint64 ChatVideoPlayer::timeSourceMs() const {
#ifdef USE_CHAT_WEBM_AUDIO
	if (m_audioIo)
		return m_sink->processedUSecs() / 1000;
#endif
	return m_wallClock.isValid() ? m_wallClock.elapsed() : 0;
}

qint64 ChatVideoPlayer::currentClockMs() const {
	return m_loopAccumMs + (timeSourceMs() - m_tsAtIterationStart);
}

void ChatVideoPlayer::toggle() {
	if (m_state == Playing)
		pause();
	else
		start();
}

void ChatVideoPlayer::start() {
	if (m_state == Playing)
		return;

	if (m_state == Poster && !ensureWorker()) {
		// The data no longer decodes (e.g. truncated) - withdraw the video.
		emit playbackUnsupported(m_url);
		return;
	}

	if (m_state == Poster) {
		// First play: the stream starts over from zero.
#ifdef USE_CHAT_WEBM_AUDIO
		if (!m_audioIo)
#endif
			m_wallClock.start();
		m_loopAccumMs        = 0;
		m_tsAtIterationStart = timeSourceMs();
	} else {
		// Resuming: re-base the clock so that time spent paused does not count
		// (the audio time source freezes on its own while the sink is suspended,
		// the wall clock does not).
		m_tsAtIterationStart = timeSourceMs();
	}

#ifdef USE_CHAT_WEBM_AUDIO
	if (m_sink && m_sink->state() == QAudio::SuspendedState)
		m_sink->resume();
#endif

	m_state = Playing;

	// Replace the paused/poster image (with its play overlay) with the clean frame.
	const QImage clean = m_lastCleanFrame.isNull() ? m_poster : m_lastCleanFrame;
	if (clean != m_displayImage) {
		m_displayImage = clean;
		emit frameReady(m_url, m_displayImage);
	}

	emit stateChanged(m_url, true);
	requestWork();
}

void ChatVideoPlayer::pause() {
	if (m_state != Playing)
		return;

	m_state = Paused;

#ifdef USE_CHAT_WEBM_AUDIO
	if (m_sink && m_sink->state() != QAudio::SuspendedState)
		m_sink->suspend();
#endif

	// Freeze the clock: the audio time source freezes on its own while the sink is
	// suspended, and the wall clock is re-based when resuming (see start()).
	m_loopAccumMs = currentClockMs();

	// Show the play overlay again so it stays discoverable that the video is paused.
	const QImage base = m_lastCleanFrame.isNull() ? m_poster : m_lastCleanFrame;
	m_displayImage    = paintPlayOverlay(base);
	emit frameReady(m_url, m_displayImage);
	emit stateChanged(m_url, false);
}

void ChatVideoPlayer::requestWork() {
	if (!m_decoder || m_workRequested)
		return;

	m_workRequested = true;
	QMetaObject::invokeMethod(m_decoder, "requestWork", Qt::QueuedConnection);
}

void ChatVideoPlayer::onDecoderVideoFrame(const QImage &image, qint64 ptsMs) {
	m_workRequested = false;
	if (m_state != Playing)
		return;

	if (m_videoQueue.size() >= MAX_QUEUED_FRAMES) {
		// The presentation fell behind - drop the oldest (late) frame.
		m_videoQueue.removeFirst();
	}
	m_videoQueue.append({ image, ptsMs });
}

void ChatVideoPlayer::onDecoderAudioChunk(const QByteArray &pcm, qint64, qint64) {
	m_workRequested = false;
#ifdef USE_CHAT_WEBM_AUDIO
	if (!m_audioIo || m_state != Playing)
		return;

	// The buffer is drained on every presentation tick; only degenerate cases
	// (a stalled sink) can make it grow beyond the cap.
	if (m_audioBuffer.size() < MAX_AUDIO_BUFFER_BYTES)
		m_audioBuffer.append(pcm);
#else
	Q_UNUSED(pcm);
#endif
}

void ChatVideoPlayer::onDecoderFinished() {
	m_workRequested = false;
	m_decoderAtEof  = true;
}

void ChatVideoPlayer::onDecoderError(const QString &message) {
	qWarning("ChatVideoPlayer: %s", qPrintable(message));
	m_workRequested = false;
	m_decoderAtEof  = true;
}

void ChatVideoPlayer::drainAudio() {
#ifdef USE_CHAT_WEBM_AUDIO
	if (!m_audioIo || m_audioBuffer.isEmpty())
		return;

	const qint64 freeBytes = m_sink->bytesFree();
	if (freeBytes <= 0)
		return;

	const qint64 toWrite = qMin< qint64 >(freeBytes, m_audioBuffer.size());
	const qint64 written = m_audioIo->write(m_audioBuffer.constData(), toWrite);
	if (written > 0)
		m_audioBuffer.remove(0, written);
#endif
}

void ChatVideoPlayer::keepDecoderFed() {
	if (m_decoderAtEof)
		return;

	const bool videoHungry = m_videoQueue.size() <= 1;
#ifdef USE_CHAT_WEBM_AUDIO
	const bool audioHungry = m_audioIo && m_audioBuffer.size() < MAX_AUDIO_BUFFER_BYTES / 3;
#else
	const bool audioHungry = false;
#endif
	if (videoHungry || audioHungry)
		requestWork();
}

void ChatVideoPlayer::maybeLoop(qint64 clockMs) {
	if (!m_decoderAtEof || !m_videoQueue.isEmpty())
		return;
#ifdef USE_CHAT_WEBM_AUDIO
	if (m_audioIo && !m_audioBuffer.isEmpty())
		return;
#endif

	const qint64 duration = m_durationMs;
	if (duration <= 0 || clockMs < m_loopAccumMs + duration)
		return;

	// Loop by mapping the stream time of the new iteration onto the current clock
	// position. Deriving the offset from the actual clock (instead of adding the
	// nominal duration) keeps loops drift-free.
	m_loopAccumMs = clockMs;
#ifdef USE_CHAT_WEBM_AUDIO
	if (!m_audioIo)
#endif
		m_wallClock.restart();
	m_tsAtIterationStart = timeSourceMs();

	m_decoderAtEof  = false;
	m_workRequested = false;
	QMetaObject::invokeMethod(m_decoder, "reopen", Qt::QueuedConnection);
	QMetaObject::invokeMethod(m_decoder, "requestWork", Qt::QueuedConnection);
}

void ChatVideoPlayer::presentTick() {
	if (m_state != Playing)
		return;

	drainAudio();

	const qint64 clockMs = currentClockMs();
	while (!m_videoQueue.isEmpty()) {
		const TimedFrame &frame = m_videoQueue.first();
		const qint64 dueMs      = frameDueMs(frame.ptsMs, m_loopAccumMs);
		if (shouldDropFrame(dueMs, clockMs)) {
			m_videoQueue.removeFirst();
			continue;
		}
		if (!shouldPresentFrame(dueMs, clockMs))
			break;

		m_lastCleanFrame = frame.image;
		m_displayImage   = frame.image;
		emit frameReady(m_url, frame.image);
		m_videoQueue.removeFirst();
	}

	keepDecoderFed();
	maybeLoop(clockMs);
}

#endif // USE_CHAT_WEBM
