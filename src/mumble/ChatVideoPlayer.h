// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHATVIDEOPLAYER_H_
#define MUMBLE_MUMBLE_CHATVIDEOPLAYER_H_

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QUrl>
#include <QtGui/QImage>

#ifdef USE_CHAT_WEBM

#ifdef USE_CHAT_WEBM_AUDIO
#	include <QtCore/QIODevice>
#	include <QtCore/QMutex>
#endif

class ChatVideoDecoder;
class QAudioSink;
class QThread;

#ifdef USE_CHAT_WEBM_AUDIO
/// Audio source for the QAudioSink, operated in pull mode: the sink reads at its
/// own pace (from the audio thread), which avoids the push-mode write sizing
/// problems of some Qt 6.4 backends (observed as garbled playback).
class ChatVideoAudioSource : public QIODevice {
public:
	explicit ChatVideoAudioSource(QObject *parent);

	/// Appends decoded audio (called from the GUI thread)
	void append(const QByteArray &pcm);
	/// Drops all buffered audio (called from the GUI thread)
	void clear();
	qint64 bufferedBytes() const;
	/// Bytes handed to the sink so far (diagnostics)
	qint64 deliveredBytes() const { return m_delivered; }

protected:
	qint64 readData(char *data, qint64 maxlen) Q_DECL_OVERRIDE;
	qint64 writeData(const char *data, qint64 maxSize) Q_DECL_OVERRIDE;

private:
	mutable QMutex m_mutex;
	QByteArray m_buffer;
	qint64 m_delivered = 0;
};
#endif

/// Plays a WebM video embedded in a chat message.
///
/// The player is the GUI-thread counterpart of ChatVideoDecoder: it owns the
/// decoder's worker thread, paces the decoded frames against a master clock and
/// feeds decoded audio into a QAudioSink. It never plays on its own - playback
/// starts and stops exclusively through toggle(), which the log widget calls
/// when the video is clicked.
///
/// A/V sync is audio-master: the playback clock is derived from the amount of
/// audio the sink has actually played (falling back to a wall clock for silent
/// videos). Video frames are presented once their timestamp is due on that
/// clock and dropped when they are too late to be worth presenting.
class ChatVideoPlayer : public QObject {
	Q_OBJECT

public:
	struct TimedFrame {
		QImage image;
		/// Presentation timestamp relative to the start of the current loop iteration
		qint64 ptsMs = -1;
	};

	/// Creates a player for the given video. The player starts out showing the
	/// poster (with a play overlay) and does not play anything until toggled.
	/// @param url The data-URL the video is registered under in the document
	/// @param videoData The raw WebM container data (retained for loop restarts)
	/// @param poster The decoded first frame of the video
	static ChatVideoPlayer *create(const QUrl &url, const QByteArray &videoData, QImage poster, QObject *parent);

	~ChatVideoPlayer() override;

	/// Toggles between playing and paused
	void toggle();
	bool isPlaying() const { return m_state == Playing; }

	/// Pauses playback (freezing the video at its current frame)
	void pause();

	/// The image currently representing the video in the document: the poster
	/// (with play overlay) while not playing, the current frame while playing.
	QImage currentImage() const { return m_displayImage; }

	// Diagnostics (used by the tests)
	/// The current playback clock in milliseconds
	qint64 currentClockMs() const;
	/// Whether decoded video frames are currently buffered for presentation
	qsizetype queuedVideoFrames() const { return m_videoQueue.size(); }
	/// Whether the decoder has decoded the whole stream already
	bool decoderAtEof() const { return m_decoderAtEof; }
	/// Whether audio is being played through a QAudioSink
	bool hasAudioOutput() const;
	/// Amount of audio data waiting to be fed into the sink (bytes)
	qsizetype bufferedAudioBytes() const;

	// Pure pacing helpers - kept static so they can be unit-tested without any
	// threads or decoders.
	/// The (absolute) time at which the given frame has to be presented
	static qint64 frameDueMs(qint64 ptsMs, qint64 loopAccumMs) { return ptsMs + loopAccumMs; }
	/// Whether a frame that is due at dueMs may be presented now
	static bool shouldPresentFrame(qint64 dueMs, qint64 clockMs) { return dueMs <= clockMs + PRESENT_AHEAD_MS; }
	/// Whether a frame that is due at dueMs is too old to be worth presenting
	static bool shouldDropFrame(qint64 dueMs, qint64 clockMs) { return dueMs < clockMs - DROP_LATE_MS; }
	/// Returns a copy of the given image with a centered play-button overlay
	static QImage paintPlayOverlay(const QImage &image);

signals:
	/// The video advanced and the given image should be shown in the document
	void frameReady(const QUrl &url, const QImage &image);
	/// The video switched between playing and paused
	void stateChanged(const QUrl &url, bool playing);
	/// The video data could not be played after all
	void playbackUnsupported(const QUrl &url);

public slots:
	/// Called by the owning LogDocument ~30 times per second while any video is
	/// playing: drains audio into the sink, presents due video frames, keeps the
	/// decoder fed and restarts the video when it ended (looping).
	void presentTick();

private slots:
	void onDecoderVideoFrame(const QImage &image, qint64 ptsMs);
	void onDecoderAudioChunk(const QByteArray &pcm, qint64 ptsMs, qint64 durationMs);
	void onDecoderFinished();
	void onDecoderError(const QString &message);

private:
	enum State { Poster, Playing, Paused };

	ChatVideoPlayer(const QUrl &url, const QByteArray &videoData, QImage poster, QObject *parent);

	void start();
	/// Sets up the decode worker thread (lazily, on first start)
	bool ensureWorker();
#ifdef USE_CHAT_WEBM_AUDIO
	/// Sets up the QAudioSink (lazily, on first start); returns whether audio is available
	bool ensureAudioSink();
#endif
	/// The raw time source the clock is based on
	qint64 timeSourceMs() const;
	/// Re-bases the clock time sources so that the current clock value stays put
	/// while the sources keep counting from here
	void rebaseClock();
	/// Requests the next bounded decode burst, if the queues run low
	void keepDecoderFed();
	/// Restarts the video from the beginning once everything has been played
	void maybeLoop(qint64 clockMs);
	void requestWork();

	const QUrl m_url;
	QByteArray m_videoData;
	State m_state = Poster;

	QImage m_poster;
	QImage m_lastCleanFrame;
	QImage m_displayImage;

	QThread *m_thread       = nullptr;
	ChatVideoDecoder *m_decoder = nullptr;
	bool m_decoderAtEof     = false;
	/// Whether a requestWork() call is in flight (avoids piling up bursts)
	bool m_workRequested    = false;

	QList< TimedFrame > m_videoQueue;

#ifdef USE_CHAT_WEBM_AUDIO
	QAudioSink *m_sink               = nullptr;
	ChatVideoAudioSource *m_audioSource = nullptr;
#endif
	/// Stream time the current loop iteration started at (0 for the first one)
	qint64 m_loopAccumMs = 0;
	/// pts of the last decoded video frame (the last thing that has to be presented)
	qint64 m_lastVideoPtsMs = 0;
	/// Whether the clock is paced by the wall clock because the audio clock ran
	/// dry (all content played out, or no audio output at all)
	bool m_wallPaced = false;
	/// Time source values when the current iteration started
	qint64 m_tsAudioBase = 0;
	qint64 m_tsWallBase  = 0;
	QElapsedTimer m_wallClock;
	/// Diagnostics: tick counter for the periodic debug line (MUMBLE_WEBM_DEBUG)
	int m_debugTick = 0;

	/// Video frames buffered ahead per video (bounded queue on the GUI side)
	static constexpr int MAX_QUEUED_FRAMES = 3;
	/// Audio buffered ahead at most (bytes; 48 kHz S16 stereo = 192000 B/s)
	static constexpr int MAX_AUDIO_BUFFER_BYTES = 192000 / 2;
	/// A frame may be presented this many milliseconds early
	static constexpr qint64 PRESENT_AHEAD_MS = 5;
	/// A frame later than this is dropped instead of presented
	static constexpr qint64 DROP_LATE_MS = 40;
};

#endif // USE_CHAT_WEBM

#endif // MUMBLE_MUMBLE_CHATVIDEOPLAYER_H_
