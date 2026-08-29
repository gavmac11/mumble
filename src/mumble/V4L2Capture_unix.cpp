// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file at the root of the Mumble source tree or at
// <https://www.mumble.info/LICENSE>.

#include "V4L2Capture.h"

#ifdef USE_SCREEN_SHARING

#	include <atomic>
#	include <mutex>
#	include <thread>

extern "C" {
#	include <libavcodec/avcodec.h>
#	include <libavdevice/avdevice.h>
#	include <libavformat/avformat.h>
#	include <libswscale/swscale.h>
#	include <libavutil/dict.h>
#	include <libavutil/error.h>
#	include <libavutil/pixfmt.h>
}

// Capture defaults. Nearly every UVC camera supports MJPEG at 720p30; on failure we fall back to
// whatever the device negotiates (e.g. YUYV).
static constexpr int CAM_WIDTH  = 1280;
static constexpr int CAM_HEIGHT = 720;
static constexpr int CAM_FPS    = 30;

struct V4L2Capture::Impl {
	std::thread worker;
	std::atomic< bool > stopRequested{ false };
	std::atomic< bool > interruptAbort{ false };
};

namespace {
/// Installed as the AVFormatContext interrupt callback so that a blocked device open or read
/// aborts promptly when capture is stopped (returns non-zero => abort).
int interruptCb(void *opaque) {
	auto *flag = static_cast< std::atomic< bool > * >(opaque);
	return flag->load(std::memory_order_relaxed) ? 1 : 0;
}

QString errString(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	av_strerror(err, buf, sizeof(buf));
	return QString::fromUtf8(buf);
}
} // namespace

V4L2Capture::~V4L2Capture() {
	stop();
	delete m_impl;
}

void V4L2Capture::start(const QString &devicePath, StartedCallback onStarted, ErrorCallback onError,
						YuvFrameCallback onYuvFrame) {
	if (m_running)
		return;

	delete m_impl;
	m_impl                = new Impl;
	m_running             = true;
	m_impl->stopRequested = false;
	m_impl->interruptAbort = false;

	m_impl->worker = std::thread([this, devicePath, onStarted, onError, onYuvFrame]() {
		run(devicePath, std::move(onStarted), std::move(onError), std::move(onYuvFrame));
		m_running = false;
	});
}

void V4L2Capture::stop() {
	if (!m_impl)
		return;
	m_impl->stopRequested = true;
	m_impl->interruptAbort = true; // unblock any pending FFmpeg call
	if (m_impl->worker.joinable())
		m_impl->worker.join();
	m_running = false;
}

void V4L2Capture::run(const QString &devicePath, StartedCallback onStarted, ErrorCallback onError,
					  YuvFrameCallback onYuvFrame) {
	const auto stopped = [this]() { return m_impl->stopRequested.load(std::memory_order_relaxed); };

	if (stopped()) {
		return;
	}

	// The video4linux2 demuxer lives in libavdevice and is invisible to av_find_input_format()
	// until it has been registered. Idempotent, so do it exactly once per process.
	static std::once_flag deviceInitOnce;
	std::call_once(deviceInitOnce, []() { avdevice_register_all(); });

	AVFormatContext *fmtCtx = nullptr;
	const AVInputFormat *iformat = av_find_input_format("video4linux2");
	if (!iformat) {
		if (onError)
			onError(QStringLiteral("FFmpeg was built without the video4linux2 input device"));
		return;
	}

	fmtCtx = avformat_alloc_context();
	fmtCtx->interrupt_callback.callback = interruptCb;
	fmtCtx->interrupt_callback.opaque   = &m_impl->interruptAbort;

	// First attempt: negotiate our preferred format. Second attempt (on failure): let the
	// driver pick whatever it supports.
	const QByteArray dev = devicePath.toUtf8();
	for (int attempt = 0; attempt < 2; ++attempt) {
		AVDictionary *opts = nullptr;
		if (attempt == 0) {
			av_dict_set(&opts, "framerate", QByteArray::number(CAM_FPS).constData(), 0);
			av_dict_set(&opts, "video_size",
						QStringLiteral("%1x%2").arg(CAM_WIDTH).arg(CAM_HEIGHT).toUtf8().constData(), 0);
			av_dict_set(&opts, "input_format", "mjpeg", 0);
		}
		const int ret = avformat_open_input(&fmtCtx, dev.constData(), iformat, &opts);
		av_dict_free(&opts);
		if (ret == 0)
			break;
		if (stopped())
			return;
		fmtCtx = nullptr; // avformat_open_input frees the context on failure
		fmtCtx = avformat_alloc_context();
		fmtCtx->interrupt_callback.callback = interruptCb;
		fmtCtx->interrupt_callback.opaque   = &m_impl->interruptAbort;
		if (attempt == 1) {
			if (onError)
				onError(QStringLiteral("Could not open camera %1: %2").arg(devicePath, errString(ret)));
			return;
		}
	}

	if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
		if (onError)
			onError(QStringLiteral("Could not read stream parameters from %1").arg(devicePath));
		avformat_close_input(&fmtCtx);
		return;
	}

	const int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (streamIdx < 0) {
		if (onError)
			onError(QStringLiteral("No video stream on %1").arg(devicePath));
		avformat_close_input(&fmtCtx);
		return;
	}
	AVStream *stream = fmtCtx->streams[streamIdx];

	const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!dec) {
		if (onError)
			onError(QStringLiteral("No decoder for camera codec %1")
						.arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id))));
		avformat_close_input(&fmtCtx);
		return;
	}
	AVCodecContext *decCtx = avcodec_alloc_context3(dec);
	if (avcodec_parameters_to_context(decCtx, stream->codecpar) < 0) {
		if (onError)
			onError(QStringLiteral("Could not configure camera decoder"));
		avcodec_free_context(&decCtx);
		avformat_close_input(&fmtCtx);
		return;
	}
	if (avcodec_open2(decCtx, dec, nullptr) < 0) {
		if (onError)
			onError(QStringLiteral("Could not open camera decoder"));
		avcodec_free_context(&decCtx);
		avformat_close_input(&fmtCtx);
		return;
	}

	AVFrame *decFrame = av_frame_alloc();
	AVFrame *yuvFrame = av_frame_alloc();
	AVPacket *pkt     = av_packet_alloc();

	bool announcedStart = false;
	int srcW = 0, srcH = 0, outW = 0, outH = 0;
	AVPixelFormat srcFmt = AV_PIX_FMT_NONE;
	struct SwsContext *sws = nullptr;

	while (!stopped()) {
		const int ret = av_read_frame(fmtCtx, pkt);
		if (ret < 0) {
			if (!stopped() && onError)
				onError(QStringLiteral("Camera read failed: %1").arg(errString(ret)));
			break;
		}
		if (pkt->stream_index != streamIdx) {
			av_packet_unref(pkt);
			continue;
		}

		if (avcodec_send_packet(decCtx, pkt) < 0) {
			av_packet_unref(pkt);
			continue;
		}
		av_packet_unref(pkt);

		while (!stopped() && avcodec_receive_frame(decCtx, decFrame) == 0) {
			srcW   = decFrame->width;
			srcH   = decFrame->height;
			srcFmt = static_cast< AVPixelFormat >(decFrame->format);

			// x264 needs even dimensions.
			const int w = srcW & ~1;
			const int h = srcH & ~1;
			if (w <= 0 || h <= 0) {
				av_frame_unref(decFrame);
				continue;
			}

			if (w != outW || h != outH || !yuvFrame->data[0]) {
				av_frame_free(&yuvFrame);
				yuvFrame          = av_frame_alloc();
				yuvFrame->format  = AV_PIX_FMT_YUV420P;
				yuvFrame->width   = w;
				yuvFrame->height  = h;
				if (av_frame_get_buffer(yuvFrame, 0) < 0)
					break;
				outW = w;
				outH = h;
			}

			sws = sws_getCachedContext(sws, srcW, srcH, srcFmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
									   nullptr, nullptr, nullptr);
			if (!sws) {
				av_frame_unref(decFrame);
				continue;
			}

			sws_scale(sws, decFrame->data, decFrame->linesize, 0, srcH, yuvFrame->data, yuvFrame->linesize);

			if (!announcedStart) {
				announcedStart = true;
				if (onStarted)
					onStarted();
			}

			if (onYuvFrame) {
				const uint8_t *planes[4] = { yuvFrame->data[0], yuvFrame->data[1], yuvFrame->data[2], nullptr };
				const int strides[4]     = { yuvFrame->linesize[0], yuvFrame->linesize[1], yuvFrame->linesize[2], 0 };
				onYuvFrame(w, h, planes, strides);
			}

			av_frame_unref(decFrame);
		}
	}

	if (sws)
		sws_freeContext(sws);
	av_frame_free(&decFrame);
	av_frame_free(&yuvFrame);
	av_packet_free(&pkt);
	avcodec_free_context(&decCtx);
	avformat_close_input(&fmtCtx);
}

#endif // USE_SCREEN_SHARING
