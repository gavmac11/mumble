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

class ChatVideoDecoder;
class QAudioSink;
class QIODevice;
class QThread;

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
	/// The current playback clock in milliseconds (audio-master, wall-clock fallback)
	qint64 currentClockMs() const;
	/// The raw time source the clock is based on
	qint64 timeSourceMs() const;
	/// Feeds buffered audio into the sink
	void drainAudio();
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
	QAudioSink *m_sink   = nullptr;
	QIODevice *m_audioIo = nullptr;
	QByteArray m_audioBuffer;
#endif
	/// Stream time the current loop iteration started at (0 for the first one)
	qint64 m_loopAccumMs = 0;
	/// Duration of the video, cached when the decoder was opened
	qint64 m_durationMs = 0;
	/// Time source value when the current iteration started
	qint64 m_tsAtIterationStart = 0;
	QElapsedTimer m_wallClock;

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
