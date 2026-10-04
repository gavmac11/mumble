// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHATVIDEODECODER_H_
#define MUMBLE_MUMBLE_CHATVIDEODECODER_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtGui/QImage>

#ifdef USE_CHAT_WEBM

extern "C" {
#	include <libavutil/channel_layout.h>
#	include <libavutil/samplefmt.h>
}

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVIOContext;
struct SwsContext;
struct SwrContext;

/// FFmpeg glue for playing WebM videos embedded in chat messages.
///
/// The decoder is an event-driven worker: it lives in its own thread and only ever
/// decodes when requestWork() is invoked on it (from the GUI thread, via a queued
/// connection). Each call produces a bounded burst of output - up to a few video
/// frames or a few hundred milliseconds of audio - and then goes back to sleep.
/// This keeps all queues on the GUI thread: the worker thread owns nothing but
/// its FFmpeg contexts and never touches the document.
class ChatVideoDecoder : public QObject {
	Q_OBJECT

public:
	struct VideoFrame {
		QImage image;
		/// Presentation timestamp of the frame, relative to the start of the stream.
		/// May be slightly negative (codec pre-roll, e.g. Opus pre-skip).
		qint64 ptsMs = 0;

		bool isValid() const { return !image.isNull(); }
	};

	struct AudioChunk {
		/// Decoded audio, resampled to S16LE, 48 kHz, stereo
		QByteArray pcm;
		/// Presentation timestamp of the first sample, relative to the start of the stream
		qint64 ptsMs = -1;
		/// Duration of the chunk in milliseconds
		qint64 durationMs = 0;

		bool isValid() const { return !pcm.isEmpty() && durationMs > 0; }
	};

	/// Whether the FFmpeg libraries this client was linked against can demux WebM
	/// (Matroska demuxer) and decode at least one of the WebM video codecs (VP8,
	/// VP9 or AV1). Individual clips are still probed separately at decode time.
	/// The result is cached.
	static bool isSupported();

	/// One-shot decode of the first video frame of the given WebM data, converted
	/// to RGBA. Used for posters, where no playback state is needed.
	/// @return The first frame, or a null image if the data could not be decoded
	static QImage decodePoster(const QByteArray &videoData);

	explicit ChatVideoDecoder(QObject *parent = nullptr);
	~ChatVideoDecoder() override;

	/// Opens the given WebM data for playback.
	/// @param videoData The raw WebM container data
	/// @param decodeAudio Whether the audio stream (if any) should be decoded as well
	bool open(const QByteArray &videoData, bool decodeAudio);
	/// Closes and frees all FFmpeg state. Safe to call from any state.
	void close();

	/// Demuxes and decodes until either a video frame or an audio chunk has been
	/// produced, or EOF is reached. Must only be called on the decoder's thread.
	/// @param videoFrame Receives the decoded video frame, if any
	/// @param audioChunk Receives the decoded audio chunk, if any
	/// @return Whether decoding can continue (false once all streams hit EOF)
	bool step(VideoFrame &videoFrame, AudioChunk &audioChunk);

	/// Duration of the video in milliseconds (0 if unknown)
	qint64 durationMs() const { return m_durationMs; }
	/// Whether the audio stream is being decoded
	bool decodingAudio() const { return m_audio.codecCtx != nullptr; }

public slots:
	/// Decodes a bounded burst of output (up to maxBurstVideoFrames video frames
	/// or maxBurstAudioMs of audio, whichever limit is hit first) and then
	/// returns, waiting for the next request. A no-op once shutdown() was called.
	void requestWork();
	/// Re-opens the retained video data from the beginning (loop restart).
	void reopen();
	/// Stops the worker for good (used before tearing the decoder down).
	void shutdown();

signals:
	void videoFrameDecoded(const QImage &image, qint64 ptsMs);
	void audioChunkDecoded(const QByteArray &pcm, qint64 ptsMs, qint64 durationMs);
	/// Emitted once all streams have been demuxed and decoded to the end
	void finished();
	void error(const QString &message);

private:
	static constexpr int MAX_BURST_VIDEO_FRAMES = 3;
	static constexpr qint64 MAX_BURST_AUDIO_MS  = 300;
	/// Output format the audio is resampled to: S16LE, 48 kHz, stereo
	static constexpr int AUDIO_OUT_SAMPLE_RATE = 48000;
	static constexpr int AUDIO_OUT_CHANNELS    = 2;

	struct StreamContext {
		AVCodecContext *codecCtx = nullptr;
		AVFrame *frame           = nullptr;
	};

	// AVIOContext callbacks reading from the in-memory video data
	static int avioReadPacket(void *opaque, uint8_t *buf, int bufSize);
	static int64_t avioSeek(void *opaque, int64_t offset, int whence);
	static void findStreams(AVFormatContext *fmtCtx, int &videoIndex, int &audioIndex);
	static bool openCodec(AVFormatContext *fmtCtx, int streamIndex, StreamContext &ctx);
	static void closeCodec(StreamContext &ctx);

	bool openInternal();
	void closeInternal();
	/// Feeds the current (or the flush) packet into the given stream's decoder
	/// and, on success, fills the respective output struct.
	/// @param flush Whether the decoder should be drained (EOF reached)
	bool decodeVideoStep(VideoFrame &videoFrame, bool flush);
	bool decodeAudioStep(AudioChunk &audioChunk, bool flush);
	qint64 streamPtsToMs(int64_t pts, int streamIndex) const;

	QByteArray m_videoData;
	bool m_decodeAudio      = false;
	qint64 m_durationMs     = 0;
	int m_videoStreamIndex  = -1;
	int m_audioStreamIndex  = -1;
	bool m_videoEof         = false;
	bool m_audioEof         = false;
	bool m_demuxEof         = false;
	bool m_shutdownRequested = false;

	AVIOContext *m_avioCtx   = nullptr;
	unsigned char *m_avioBuf = nullptr;
	qint64 m_avioPos         = 0;

	AVFormatContext *m_fmtCtx = nullptr;
	AVPacket *m_packet        = nullptr;
	StreamContext m_video;
	StreamContext m_audio;
	SwsContext *m_swsCtx      = nullptr;
	int m_swsWidth = 0, m_swsHeight = 0;
	SwrContext *m_swrCtx     = nullptr;
	/// Parameters the current m_swrCtx was created for (SwrContext is opaque)
	int m_swrInRate          = 0;
	AVSampleFormat m_swrInFormat = AV_SAMPLE_FMT_NONE;
	AVChannelLayout m_swrInLayout = {};
};

#endif // USE_CHAT_WEBM

#endif // MUMBLE_MUMBLE_CHATVIDEODECODER_H_
