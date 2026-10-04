// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatVideoDecoder.h"

#ifdef USE_CHAT_WEBM

// The FFmpeg headers have to be wrapped in extern "C" explicitly (they do not
// do so themselves). PaddedImage.h includes libavutil/mem.h and therefore has
// to come after this block as well, so that declaration linkage is consistent.
extern "C" {
#	include <libavcodec/avcodec.h>
#	include <libavformat/avformat.h>
#	include <libswresample/swresample.h>
#	include <libswscale/swscale.h>
}

#include "PaddedImage.h"

#include <cstring>

namespace {
/// Size of the AVIOContext buffer for the custom in-memory data source
constexpr int AVIO_BUFFER_SIZE = 32 * 1024;
} // namespace

ChatVideoDecoder::ChatVideoDecoder(QObject *parent) : QObject(parent) {
}

ChatVideoDecoder::~ChatVideoDecoder() {
	close();
}

bool ChatVideoDecoder::isSupported() {
	static const bool supported = []() {
		// One of the video codecs has to be available; clips using the others then
		// simply fail to decode and degrade to the placeholder individually.
		return av_find_input_format("matroska")
			   && (avcodec_find_decoder(AV_CODEC_ID_VP8) || avcodec_find_decoder(AV_CODEC_ID_VP9)
				   || avcodec_find_decoder(AV_CODEC_ID_AV1));
	}();
	return supported;
}

QImage ChatVideoDecoder::decodePoster(const QByteArray &videoData) {
	if (!isSupported() || videoData.isEmpty())
		return QImage();

	ChatVideoDecoder decoder;
	if (!decoder.open(videoData, false))
		return QImage();

	VideoFrame frame;
	AudioChunk audio;
	while (decoder.step(frame, audio)) {
		if (frame.isValid())
			return frame.image;
	}
	return QImage();
}

int ChatVideoDecoder::avioReadPacket(void *opaque, uint8_t *buf, int bufSize) {
	auto *self         = static_cast< ChatVideoDecoder * >(opaque);
	const qint64 avail = self->m_videoData.size() - self->m_avioPos;
	const int toRead   = static_cast< int >(qMin< qint64 >(avail, bufSize));
	if (toRead > 0) {
		std::memcpy(buf, self->m_videoData.constData() + self->m_avioPos, static_cast< size_t >(toRead));
		self->m_avioPos += toRead;
	}
	return toRead == 0 ? AVERROR_EOF : toRead;
}

int64_t ChatVideoDecoder::avioSeek(void *opaque, int64_t offset, int whence) {
	auto *self = static_cast< ChatVideoDecoder * >(opaque);
	switch (whence) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			offset += self->m_avioPos;
			break;
		case SEEK_END:
			offset += self->m_videoData.size();
			break;
		case AVSEEK_SIZE:
			return self->m_videoData.size();
		default:
			return -1;
	}
	if (offset < 0 || offset > self->m_videoData.size())
		return -1;
	self->m_avioPos = offset;
	return offset;
}

void ChatVideoDecoder::findStreams(AVFormatContext *fmtCtx, int &videoIndex, int &audioIndex) {
	videoIndex = audioIndex = -1;
	for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
		const AVMediaType type = fmtCtx->streams[i]->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO && videoIndex == -1)
			videoIndex = static_cast< int >(i);
		else if (type == AVMEDIA_TYPE_AUDIO && audioIndex == -1)
			audioIndex = static_cast< int >(i);
	}
}

bool ChatVideoDecoder::openCodec(AVFormatContext *fmtCtx, int streamIndex, StreamContext &ctx) {
	const AVCodec *decoder = avcodec_find_decoder(fmtCtx->streams[streamIndex]->codecpar->codec_id);
	if (!decoder)
		return false;

	ctx.codecCtx = avcodec_alloc_context3(decoder);
	if (!ctx.codecCtx)
		return false;

	if (avcodec_parameters_to_context(ctx.codecCtx, fmtCtx->streams[streamIndex]->codecpar) < 0) {
		avcodec_free_context(&ctx.codecCtx);
		return false;
	}

	// A chat log can hold several playing videos; a single decode thread per video
	// keeps the total thread count bounded.
	ctx.codecCtx->thread_count = 1;

	if (avcodec_open2(ctx.codecCtx, decoder, nullptr) < 0) {
		avcodec_free_context(&ctx.codecCtx);
		return false;
	}

	ctx.frame = av_frame_alloc();
	return ctx.frame != nullptr;
}

void ChatVideoDecoder::closeCodec(StreamContext &ctx) {
	if (ctx.frame) {
		av_frame_free(&ctx.frame);
	}
	if (ctx.codecCtx) {
		avcodec_free_context(&ctx.codecCtx);
	}
}

bool ChatVideoDecoder::open(const QByteArray &videoData, bool decodeAudio) {
	// Keep a reference to the data (QByteArrays share their buffer, so this is
	// cheap) so that reopen() can start over without the caller's help.
	m_videoData  = videoData;
	m_decodeAudio = decodeAudio;
	return openInternal();
}

bool ChatVideoDecoder::reopen() {
	closeInternal();
	return openInternal();
}

void ChatVideoDecoder::close() {
	closeInternal();
	m_videoData.clear();
	m_decodeAudio = false;
}

bool ChatVideoDecoder::openInternal() {
	m_avioBuf = static_cast< unsigned char * >(av_malloc(AVIO_BUFFER_SIZE));
	if (!m_avioBuf)
		return false;

	m_avioCtx = avio_alloc_context(m_avioBuf, AVIO_BUFFER_SIZE, 0, this, &ChatVideoDecoder::avioReadPacket, nullptr,
								   &ChatVideoDecoder::avioSeek);
	if (!m_avioCtx) {
		av_free(m_avioBuf);
		m_avioBuf = nullptr;
		return false;
	}

	m_fmtCtx = avformat_alloc_context();
	if (!m_fmtCtx) {
		avio_context_free(&m_avioCtx);
		return false;
	}
	m_fmtCtx->pb     = m_avioCtx;
	m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

	// Note: on success, avformat_open_input takes ownership of the AVIOContext
	// (avformat_close_input frees it, including its buffer). On failure it does
	// not, so it has to be freed manually below.
	const AVInputFormat *format = av_find_input_format("matroska");
	if (avformat_open_input(&m_fmtCtx, "", format, nullptr) < 0) {
		avformat_free_context(m_fmtCtx);
		avio_context_free(&m_avioCtx);
		return false;
	}

	m_packet = av_packet_alloc();
	if (!m_packet || avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
		closeInternal();
		return false;
	}

	findStreams(m_fmtCtx, m_videoStreamIndex, m_audioStreamIndex);
	if (m_videoStreamIndex == -1) {
		closeInternal();
		return false;
	}

	if (!openCodec(m_fmtCtx, m_videoStreamIndex, m_video)) {
		closeInternal();
		return false;
	}

	if (m_decodeAudio && m_audioStreamIndex != -1) {
		// A missing audio decoder (e.g. an Opus-less FFmpeg build) is not fatal -
		// the video then simply plays without sound.
		openCodec(m_fmtCtx, m_audioStreamIndex, m_audio);
	}

	if (m_fmtCtx->duration > 0) {
		m_durationMs = m_fmtCtx->duration * 1000 / AV_TIME_BASE;
	}

	m_videoEof = m_audioEof = m_demuxEof = false;
	return true;
}

void ChatVideoDecoder::closeInternal() {
	if (m_swsCtx) {
		sws_freeContext(m_swsCtx);
		m_swsCtx = nullptr;
	}
	if (m_swrCtx) {
		swr_free(&m_swrCtx);
		av_channel_layout_uninit(&m_swrInLayout);
		m_swrInRate   = 0;
		m_swrInFormat = AV_SAMPLE_FMT_NONE;
	}
	closeCodec(m_video);
	closeCodec(m_audio);
	if (m_packet) {
		av_packet_free(&m_packet);
	}
	if (m_fmtCtx) {
		avformat_close_input(&m_fmtCtx);
	} else if (m_avioCtx) {
		// The context was never handed over to the demuxer - free it ourselves.
		avio_context_free(&m_avioCtx);
	}
	m_videoStreamIndex = m_audioStreamIndex = -1;
	m_durationMs                           = 0;
	m_videoEof = m_audioEof = m_demuxEof = false;
	m_swsWidth = m_swsHeight             = 0;
}

void ChatVideoDecoder::requestWork() {
	if (m_shutdownRequested)
		return;

	int videoProduced    = 0;
	qint64 audioProduced = 0;
	while (!m_shutdownRequested && videoProduced < MAX_BURST_VIDEO_FRAMES && audioProduced < MAX_BURST_AUDIO_MS) {
		VideoFrame videoFrame;
		AudioChunk audioChunk;
		if (!step(videoFrame, audioChunk)) {
			emit finished();
			return;
		}
		if (videoFrame.isValid()) {
			emit videoFrameDecoded(videoFrame.image, videoFrame.ptsMs);
			++videoProduced;
		}
		if (audioChunk.isValid()) {
			emit audioChunkDecoded(audioChunk.pcm, audioChunk.ptsMs, audioChunk.durationMs);
			audioProduced += audioChunk.durationMs;
		}
	}
}

void ChatVideoDecoder::shutdown() {
	m_shutdownRequested = true;
}

bool ChatVideoDecoder::step(VideoFrame &videoFrame, AudioChunk &audioChunk) {
	if (!m_fmtCtx)
		return false;

	while (true) {
		// Drain the decoders once all packets have been fed to them.
		if (m_demuxEof) {
			if (!m_videoEof && decodeVideoStep(videoFrame, true))
				return true;
			m_videoEof = true;
			if (m_audio.codecCtx && !m_audioEof && decodeAudioStep(audioChunk, true))
				return true;
			m_audioEof = true;
			return false;
		}

		av_packet_unref(m_packet);
		const int ret = av_read_frame(m_fmtCtx, m_packet);
		if (ret == AVERROR_EOF) {
			m_demuxEof = true;
			continue;
		}
		if (ret < 0) {
			emit error(tr("Failed to read video data (error %1)").arg(ret));
			m_demuxEof = true;
			continue;
		}

		if (m_packet->stream_index == m_videoStreamIndex) {
			if (decodeVideoStep(videoFrame, false))
				return true;
		} else if (m_audio.codecCtx && m_packet->stream_index == m_audioStreamIndex) {
			if (decodeAudioStep(audioChunk, false))
				return true;
		}
		// Nothing was produced (e.g. a packet that only buffered decoder state) -
		// keep going with the next packet.
	}
}

qint64 ChatVideoDecoder::streamPtsToMs(int64_t pts, int streamIndex) const {
	const AVRational &timeBase = m_fmtCtx->streams[streamIndex]->time_base;
	return static_cast< qint64 >(std::llround(static_cast< double >(pts) * 1000.0 * timeBase.num / timeBase.den));
}

bool ChatVideoDecoder::decodeVideoStep(VideoFrame &videoFrame, bool flush) {
	int ret = avcodec_send_packet(m_video.codecCtx, flush ? nullptr : m_packet);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return false;

	while ((ret = avcodec_receive_frame(m_video.codecCtx, m_video.frame)) == 0) {
		if (m_video.frame->best_effort_timestamp == AV_NOPTS_VALUE)
			continue;

		const int width  = m_video.frame->width;
		const int height = m_video.frame->height;

		// (Re-)create the sws context if dimensions changed (mirrors
		// ScreenShareReceiver::decodeCompleteFrame).
		if (!m_swsCtx || m_swsWidth != width || m_swsHeight != height) {
			if (m_swsCtx)
				sws_freeContext(m_swsCtx);
			m_swsCtx   = sws_getContext(width, height, static_cast< AVPixelFormat >(m_video.frame->format), width,
										  height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
			m_swsWidth  = width;
			m_swsHeight = height;
		}
		if (!m_swsCtx)
			continue;

		// The destination is allocated with padded rows and tail slack: swscale's
		// SIMD tails can overshoot the last row for some widths (see PaddedImage.h).
		QImage image = allocatePaddedRGBAImage(width, height);
		if (image.isNull())
			continue;
		uint8_t *dstData[1] = { image.bits() };
		int dstStride[1]    = { static_cast< int >(image.bytesPerLine()) };
		sws_scale(m_swsCtx, m_video.frame->data, m_video.frame->linesize, 0, height, dstData, dstStride);

		videoFrame.image = image;
		videoFrame.ptsMs = streamPtsToMs(m_video.frame->best_effort_timestamp, m_videoStreamIndex);
		return true;
	}
	return false;
}

bool ChatVideoDecoder::decodeAudioStep(AudioChunk &audioChunk, bool flush) {
	int ret = avcodec_send_packet(m_audio.codecCtx, flush ? nullptr : m_packet);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return false;

	while ((ret = avcodec_receive_frame(m_audio.codecCtx, m_audio.frame)) == 0) {
		if (m_audio.frame->best_effort_timestamp == AV_NOPTS_VALUE)
			continue;

		// (Re-)create the resampler whenever the input parameters change. SwrContext
		// is opaque, so the last-used parameters are tracked here.
		const bool layoutMatches = m_swrInLayout.order == AV_CHANNEL_ORDER_UNSPEC
									   ? m_audio.frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
									   : av_channel_layout_compare(&m_swrInLayout, &m_audio.frame->ch_layout) == 0;
		if (!m_swrCtx || m_swrInRate != m_audio.frame->sample_rate
			|| m_swrInFormat != static_cast< AVSampleFormat >(m_audio.frame->format) || !layoutMatches) {
			swr_free(&m_swrCtx);
			av_channel_layout_uninit(&m_swrInLayout);
			AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
			if (swr_alloc_set_opts2(&m_swrCtx, &m_audio.frame->ch_layout,
									static_cast< AVSampleFormat >(m_audio.frame->format),
									m_audio.frame->sample_rate, &outLayout, AV_SAMPLE_FMT_S16,
									AUDIO_OUT_SAMPLE_RATE, 0, nullptr)
				< 0) {
				m_swrCtx = nullptr;
				continue;
			}
			m_swrInRate    = m_audio.frame->sample_rate;
			m_swrInFormat  = static_cast< AVSampleFormat >(m_audio.frame->format);
			// Custom channel orders own heap data and must not be copied.
			if (m_audio.frame->ch_layout.order != AV_CHANNEL_ORDER_CUSTOM)
				m_swrInLayout = m_audio.frame->ch_layout;
		}

		const int maxOutSamples = swr_get_out_samples(m_swrCtx, m_audio.frame->nb_samples);
		if (maxOutSamples <= 0)
			continue;

		constexpr qint64 BYTES_PER_SAMPLE = AUDIO_OUT_CHANNELS * static_cast< qint64 >(sizeof(int16_t));
		QByteArray pcm(maxOutSamples * BYTES_PER_SAMPLE, Qt::Uninitialized);
		uint8_t *outPtr[1] = { reinterpret_cast< uint8_t * >(pcm.data()) };
		const int outSamples =
			swr_convert(m_swrCtx, outPtr, maxOutSamples, m_audio.frame->extended_data, m_audio.frame->nb_samples);
		if (outSamples <= 0)
			continue;

		pcm.resize(outSamples * BYTES_PER_SAMPLE);
		audioChunk.pcm        = pcm;
		audioChunk.ptsMs      = streamPtsToMs(m_audio.frame->best_effort_timestamp, m_audioStreamIndex);
		audioChunk.durationMs = static_cast< qint64 >(outSamples) * 1000 / AUDIO_OUT_SAMPLE_RATE;
		return true;
	}
	return false;
}

#endif // USE_CHAT_WEBM
