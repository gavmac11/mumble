// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatVideoPlayer.h"

#ifdef USE_CHAT_WEBM

#include "ChatVideoDecoder.h"

#include <cstring>

#include <QtCore/QThread>
#include <QtGui/QPainter>
#include <QtGui/QPolygonF>

#ifdef USE_CHAT_WEBM_AUDIO
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QMediaDevices>
#endif

namespace {
/// Chat video playback diagnostics are logged when MUMBLE_WEBM_DEBUG is set
bool webmDebugEnabled() {
	static const bool enabled = qEnvironmentVariableIsSet("MUMBLE_WEBM_DEBUG");
	return enabled;
}

#ifdef USE_CHAT_WEBM_AUDIO
/// Dumps the PCM that is fed into the audio sink, for debugging (MUMBLE_WEBM_DEBUG)
void dumpAudioChunk(const QByteArray &pcm) {
	static QFile dump(QStringLiteral("/tmp/webm_pcm_%1.raw").arg(QCoreApplication::applicationPid()));
	if (!dump.isOpen()) {
		dump.open(QIODevice::WriteOnly | QIODevice::Append);
	}
	dump.write(pcm);
}
#endif // USE_CHAT_WEBM_AUDIO
} // namespace

#ifdef USE_CHAT_WEBM_AUDIO
ChatVideoAudioSource::ChatVideoAudioSource(QObject *parent) : QIODevice(parent) {
	open(ReadOnly);
}

void ChatVideoAudioSource::append(const QByteArray &pcm) {
	QMutexLocker lock(&m_mutex);
	m_buffer.append(pcm);
}

void ChatVideoAudioSource::clear() {
	QMutexLocker lock(&m_mutex);
	m_buffer.clear();
}

qint64 ChatVideoAudioSource::bufferedBytes() const {
	QMutexLocker lock(&m_mutex);
	return m_buffer.size();
}

qint64 ChatVideoAudioSource::readData(char *data, qint64 maxlen) {
	QMutexLocker lock(&m_mutex);
	const qint64 toCopy = qMin< qint64 >(maxlen, m_buffer.size());
	if (toCopy > 0) {
		std::memcpy(data, m_buffer.constData(), static_cast< size_t >(toCopy));
		m_buffer.remove(0, static_cast< int >(toCopy));
		m_delivered += toCopy;
	}
	return toCopy;
}

qint64 ChatVideoAudioSource::writeData(const char *, qint64) {
	return 0;
}
#endif // USE_CHAT_WEBM_AUDIO

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
		m_sink        = nullptr;
		m_audioSource = nullptr;  // child of this, deleted with the player
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
			m_sink         = nullptr;
			m_audioSource  = nullptr;
		}
#endif
		return false;
	}

#ifdef USE_CHAT_WEBM_AUDIO
	if (decodeAudio && !m_decoder->decodingAudio()) {
		// The clip carries no decodable audio (a silent video, or an audio codec
		// this FFmpeg build lacks). The audio clock would never advance in that
		// case - the sink cannot report anything as played - so playback is paced
		// against the wall clock from the start instead.
		m_sink->stop();
		delete m_sink;
		m_sink        = nullptr;
		m_audioSource = nullptr;  // child of this, deleted with the player
		m_wallPaced   = true;
	}
#endif

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
		return m_audioSource != nullptr;

	QAudioFormat format;
	format.setSampleFormat(QAudioFormat::Int16);
	format.setSampleRate(48000);
	format.setChannelCount(2);

	// The sink has to be bound to the default output device explicitly: the
	// device-less constructor does not reliably pick the system default (observed
	// with Qt 6.4: it opened the first device in the list - HDMI - instead).
	const QAudioDevice device = QMediaDevices::defaultAudioOutput();
	if (device.isNull()) {
		qWarning("ChatVideoPlayer: no audio output device - playing without sound");
		return false;
	}

	// Pull mode: the sink reads from the source at its own pace.
	m_audioSource = new ChatVideoAudioSource(this);
	QAudioSink *sink = new QAudioSink(device, format, this);
	sink->start(m_audioSource);
	if (sink->error() == QAudio::FatalError) {
		qWarning("ChatVideoPlayer: unable to open an audio output - playing without sound");
		sink->stop();
		delete sink;
		m_audioSource->deleteLater();
		m_audioSource = nullptr;
		return false;
	}
	if (webmDebugEnabled()) {
		qInfo("ChatVideoPlayer: audio output opened (pull mode, bufferSize=%lld)",
			  static_cast< long long >(sink->bufferSize()));
	}

	connect(sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State state) {
		if (webmDebugEnabled()) {
			qInfo("ChatVideoPlayer: sink state -> %d (error %d)", static_cast< int >(state),
				  static_cast< int >(sink->error()));
		}
		// Underruns are routine (the source may briefly run dry, e.g. at loop
		// seams) and are handled by reviving the sink - not an error.
		if (sink->error() != QAudio::NoError && sink->error() != QAudio::UnderrunError && m_state == Playing) {
			qWarning("ChatVideoPlayer: audio output error - continuing without sound");
			// Fall back to the wall clock, continuing from the current position.
			// (Only the clock base moves - the frame schedule stays put.)
			m_clockBaseMs = currentClockMs();
			m_audioSource->clear();
			m_audioSource = nullptr;
			m_wallPaced   = true;
			rebaseClock();
			// Detach the broken sink (stopped and deleted at destruction).
			sink->disconnect(this);
		}
	});

	m_sink = sink;
	return true;
}
#endif

qint64 ChatVideoPlayer::timeSourceMs() const {
#ifdef USE_CHAT_WEBM_AUDIO
	// The amount of audio the sink actually played is the master clock. It only
	// ever gets consulted while m_wallPaced is false - once the audio ran dry
	// (everything played out), the wall clock takes over, see presentTick().
	if (!m_wallPaced && m_audioSource)
		return m_sink->processedUSecs() / 1000;
#endif
	return m_wallClock.isValid() ? m_wallClock.elapsed() : 0;
}

void ChatVideoPlayer::rebaseClock() {
	m_tsAudioBase = 0;
#ifdef USE_CHAT_WEBM_AUDIO
	if (m_audioSource)
		m_tsAudioBase = m_sink->processedUSecs() / 1000;
#endif
	if (!m_wallClock.isValid())
		m_wallClock.start();
	m_tsWallBase = m_wallClock.elapsed();
}

qint64 ChatVideoPlayer::currentClockMs() const {
	const qint64 source = timeSourceMs();
	qint64 base;
#ifdef USE_CHAT_WEBM_AUDIO
	base = (m_audioSource && !m_wallPaced) ? m_tsAudioBase : m_tsWallBase;
#else
	base = m_tsWallBase;
#endif
	return m_clockBaseMs + (source - base);
}

bool ChatVideoPlayer::hasAudioOutput() const {
#ifdef USE_CHAT_WEBM_AUDIO
	return m_audioSource != nullptr;
#else
	return false;
#endif
}

qsizetype ChatVideoPlayer::bufferedAudioBytes() const {
#ifdef USE_CHAT_WEBM_AUDIO
	return m_audioSource ? m_audioSource->bufferedBytes() : 0;
#else
	return 0;
#endif
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
		m_loopAccumMs    = 0;
		m_clockBaseMs    = 0;
		m_lastVideoPtsMs = 0;
		// Without a working audio clock (a sink could not be opened, or the clip
		// has no decodable audio) the wall clock paces playback from the start.
		m_wallPaced = !hasAudioOutput();
	} else if (m_decoderAtEof && m_videoQueue.isEmpty()
#ifdef USE_CHAT_WEBM_AUDIO
			   && (!m_audioSource || bufferedAudioBytes() == 0)
#endif
	) {
		// Resuming at (or right past) the end of the content: all frames have
		// been presented already, so the loop gate would hold the restart back
		// until the clock coasts to its threshold - up to a full clip duration
		// of frozen video. Dropping the threshold lets the loop fire on the very
		// next presentation tick instead.
		m_lastVideoPtsMs = 0;
	}
	// Resuming: m_loopAccumMs (the anchor of the frame due times) keeps the value
	// of the current iteration; re-basing discards whatever the wall clock counted
	// in the meantime (the audio time source froze on its own while the sink was
	// suspended) and the clock base latched at pause time restores the position.
	rebaseClock();

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

	// Freeze the clock: the audio time source stops on its own while the sink is
	// suspended, and re-basing on resume resets both time sources' contributions
	// to zero. The pre-pause clock value is therefore latched into the clock base
	// here. The frame due times (pts + m_loopAccumMs) are deliberately left
	// alone: m_loopAccumMs anchors the frame schedule of the current iteration
	// and must not absorb the time already played, or every queued frame's due
	// time would shift by it and the video would freeze for that span after
	// resuming.
	m_clockBaseMs = currentClockMs();

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

	m_lastVideoPtsMs = qMax(m_lastVideoPtsMs, ptsMs);
	if (m_videoQueue.size() >= MAX_QUEUED_FRAMES) {
		// The presentation fell behind - drop the oldest (late) frame.
		m_videoQueue.removeFirst();
	}
	m_videoQueue.append({ image, ptsMs });
}

void ChatVideoPlayer::onDecoderAudioChunk(const QByteArray &pcm, qint64, qint64) {
	m_workRequested = false;
#ifdef USE_CHAT_WEBM_AUDIO
	if (!m_audioSource || m_state != Playing)
		return;

	// The sink pulls from the source at its own pace; only degenerate cases (a
	// stalled sink) can make the buffer grow beyond the cap.
	if (m_audioSource->bufferedBytes() < MAX_AUDIO_BUFFER_BYTES)
		m_audioSource->append(pcm);
	if (webmDebugEnabled()) {
		dumpAudioChunk(pcm);
	}
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

void ChatVideoPlayer::keepDecoderFed() {
	if (m_decoderAtEof)
		return;

	const bool videoHungry = m_videoQueue.size() <= 1;
#ifdef USE_CHAT_WEBM_AUDIO
	const bool audioHungry = m_audioSource && bufferedAudioBytes() < MAX_AUDIO_BUFFER_BYTES / 3;
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
	if (m_audioSource && bufferedAudioBytes() > 0)
		return;
#endif

	// Loop once the last decoded frame is due - the last thing that still has to
	// be presented. The container's nominal duration is deliberately not used
	// here: the audio clock stops as soon as the sink runs dry, which can happen
	// slightly before (or after) that duration, which would deadlock the loop.
	if (clockMs < m_loopAccumMs + m_lastVideoPtsMs)
		return;

	// Loop by mapping the stream time of the new iteration onto the current clock
	// position. Deriving the offset from the actual clock (instead of adding the
	// nominal duration) keeps loops drift-free.
	m_loopAccumMs = clockMs;
	m_clockBaseMs = clockMs;
	m_wallPaced   = false;
	rebaseClock();

	m_decoderAtEof  = false;
	m_workRequested = false;
	if (webmDebugEnabled()) {
		qInfo("ChatVideoPlayer: looping (clock=%lldms)", static_cast< long long >(clockMs));
	}
	const bool reopenQueued = QMetaObject::invokeMethod(m_decoder, "reopen", Qt::QueuedConnection);
	QMetaObject::invokeMethod(m_decoder, "requestWork", Qt::QueuedConnection);
	if (!reopenQueued && webmDebugEnabled()) {
		qInfo("ChatVideoPlayer: FAILED to queue reopen invocation");
	}
}

void ChatVideoPlayer::presentTick() {
	if (m_state != Playing)
		return;

#ifdef USE_CHAT_WEBM_AUDIO
	// Pull-mode sinks do not resume pulling by themselves after an underrun (the
	// sink went Idle and never reads the source again). A fresh sink object is
	// created instead of restarting the old one - cycling stop()/start() on a
	// live sink proved fragile. The clock is re-based around the restart so
	// playback continues seamlessly.
	if (m_audioSource && !m_wallPaced && m_sink->state() == QAudio::IdleState
		&& bufferedAudioBytes() > 0) {
		if (webmDebugEnabled()) {
			qInfo("ChatVideoPlayer: reviving idle audio sink");
		}
		// Only the clock base moves (the fresh sink's time source starts over);
		// the frame schedule stays put.
		m_clockBaseMs = currentClockMs();
		m_sink->disconnect(this);
		m_sink->stop();
		delete m_sink;
		m_sink = nullptr;
		if (!ensureAudioSink()) {
			// Keep playing on the wall clock if a fresh sink cannot be opened.
			m_wallPaced = true;
			rebaseClock();
		} else {
			rebaseClock();
		}
	}

	// All content is decoded and played out - the audio clock would stall from
	// here on (the sink is dry). Switch to the wall clock, offset to the current
	// position, so that the last frames can still become due and looping can
	// kick in.
	if (!m_wallPaced && m_audioSource && m_decoderAtEof && bufferedAudioBytes() == 0
		&& m_sink->state() != QAudio::ActiveState) {
		m_wallPaced   = true;
		m_clockBaseMs += m_sink->processedUSecs() / 1000 - m_tsAudioBase;
		rebaseClock();
		if (webmDebugEnabled()) {
			qInfo("ChatVideoPlayer: audio clock ran dry - switching to wall clock at %lldms",
				  static_cast< long long >(m_clockBaseMs));
		}
	}
#endif

	const qint64 clockMs = currentClockMs();
	if (webmDebugEnabled() && (++m_debugTick % 33) == 0) {
#ifdef USE_CHAT_WEBM_AUDIO
		qInfo("ChatVideoPlayer: tick clock=%lldms queue=%lld audioBuf=%lld delivered=%lld eof=%d",
			  static_cast< long long >(clockMs), static_cast< long long >(m_videoQueue.size()),
			  static_cast< long long >(bufferedAudioBytes()),
			  static_cast< long long >(m_audioSource ? m_audioSource->deliveredBytes() : 0),
			  m_decoderAtEof ? 1 : 0);
#else
		qInfo("ChatVideoPlayer: tick clock=%lldms queue=%lld eof=%d",
			  static_cast< long long >(clockMs), static_cast< long long >(m_videoQueue.size()),
			  m_decoderAtEof ? 1 : 0);
#endif
	}
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
