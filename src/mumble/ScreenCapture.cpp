// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenCapture.h"

#include "Log.h"

#ifdef USE_SCREEN_SHARING
#	include "CaptureSourceLister.h"
#	include "PaddedImage.h"
#	include <QtCore/QPointer>
#	include <QtGui/QImage>
#	ifdef Q_OS_MAC
#		include "AVFCapture.h"
#		include "SCKitCapture.h"
#	elif defined(HAS_WAYLAND_PORTAL)
#		include "XdgPortalCapture.h"
#	endif
#endif

#include "Global.h"

static constexpr int CAPTURE_INTERVAL_MS = 66; // ~15 fps
/// Self-preview frame pacing and size cap. The preview is for framing, not monitoring, so a
/// few frames per second at a modest resolution are plenty — and keep the webcam worker
/// thread's extra conversion cheap.
static constexpr int PREVIEW_FRAME_INTERVAL_MS = 100; // ~10 fps
static constexpr int PREVIEW_MAX_WIDTH         = 640;

ScreenCapture::ScreenCapture(QObject *parent) : QObject(parent) {
	m_captureTimer = new QTimer(this);
	m_captureTimer->setInterval(CAPTURE_INTERVAL_MS);
	connect(m_captureTimer, &QTimer::timeout, this, &ScreenCapture::captureFrame);
}

ScreenCapture::~ScreenCapture() {
	stopCapture();
#ifdef USE_SCREEN_SHARING
	// stopCapture() early-returns when nothing was capturing (e.g. after a webcam error), so the
	// preview scaler has to be released here as well.
	freePreviewScaler();
#	if defined(Q_OS_LINUX)
	delete m_v4l2;
	m_v4l2 = nullptr;
#	endif
#endif
}

void ScreenCapture::startCapture() {
#ifndef USE_SCREEN_SHARING
	// This way it's sent to the chatbox. I don't know if this should be a qWarning instead.
	Global::get().l->log(Log::Warning,
						 QObject::tr("Screen sharing requires Mumble to be built with -Dscreen-sharing=ON."));
#else
	if (m_capturing)
		return;

	m_reportedCaptureStarted = false;

#	if defined(Q_OS_LINUX)
	if (m_source.type == CaptureSource::Type::Webcam) {
		m_frameNumber     = 0;
		m_lastPreviewEmit = {};

		QPointer< ScreenCapture > self = this;
		auto onStarted                = [self]() {
            if (!self)
                return;
            self->m_capturing = true;
		};
		auto onError = [self](QString error) {
			if (!self)
				return;
			Global::get().l->log(Log::Warning, QObject::tr("Webcam capture failed: %1").arg(error));
			self->m_capturing = false;
			self->destroyEncoder();
			emit self->captureAborted();
			// This path bypasses stopCapture() (the worker cannot join itself), so announce the
			// stop here as well for anything tracking capture state, e.g. the self-share preview.
			emit self->captureStopped();
		};
		auto onFrame = [self](int width, int height, const uint8_t *const data[4], const int linesize[4]) {
			if (!self || !self->m_capturing)
				return;
			self->encodeYuvFrame(width, height, data, linesize);
		};

		if (!m_v4l2)
			m_v4l2 = new V4L2Capture();
		m_v4l2->start(m_source.devicePath, std::move(onStarted), std::move(onError), std::move(onFrame));
		return;
	}
#	endif

	if (m_source.type == CaptureSource::Type::Webcam) {
#	if defined(Q_OS_MAC)
		m_frameNumber     = 0;
		m_lastPreviewEmit = {};
		m_capturing       = true;
		QPointer< ScreenCapture > self = this;
		auto onStarted                = [self]() {
			if (self)
				self->m_capturing = true;
		};
		auto onError = [self](QString error) {
			if (!self)
				return;
			Global::get().l->log(Log::Warning, QObject::tr("Webcam capture failed: %1").arg(error));
			self->stopCapture();
			emit self->captureAborted();
		};
		auto onFrame = [self](QImage frame) {
			if (self && self->m_capturing)
				self->encodeImage(frame);
		};
		avf_startCamera(m_source.devicePath, std::move(onStarted), std::move(onError), std::move(onFrame));
		return;
#	endif
	}

	m_frameNumber     = 0;
	m_lastPreviewEmit = {};
	m_capturing       = true;
	m_captureTimer->start();
#endif
}

void ScreenCapture::stopCapture() {
#ifdef USE_SCREEN_SHARING
#	if defined(Q_OS_MAC)
	// Always invalidate camera callbacks first; permission or session startup may still be pending.
	avf_stopCamera();
#	endif
#	if defined(Q_OS_LINUX)
	// Always stop the camera first — it may still be starting up with m_capturing == false,
	// and the worker thread must be joined before the encoder it uses is torn down.
	if (m_v4l2) {
		m_v4l2->stop();
	}
#	endif
#endif

	if (!m_capturing)
		return;

	m_captureTimer->stop();
	m_capturing = false;

#ifdef USE_SCREEN_SHARING
#	ifdef Q_OS_MAC
	sckit_stop();
#	elif defined(HAS_WAYLAND_PORTAL)
	xdg_portal_stop();
#	endif
	destroyEncoder();
	// Safe to release here: any capture worker (V4L2) has been joined above, so nothing can be
	// inside convertPreviewFrame() anymore.
	freePreviewScaler();
#endif
	emit captureStopped();
}

bool ScreenCapture::isCapturing() const {
	return m_capturing;
}

#ifdef USE_SCREEN_SHARING

void ScreenCapture::setSource(const CaptureSource &source) {
	m_source = source;
	destroyEncoder(); // Reset so the encoder reinitialises at the new source's resolution.
}

#	if defined(Q_OS_MAC) || defined(HAS_WAYLAND_PORTAL)
void ScreenCapture::startCaptureNative() {
	if (m_capturing)
		return;

	m_reportedCaptureStarted = false;

	// Keep a safe pointer — the lambdas below must not capture `this` without guard.
	QPointer< ScreenCapture > self = this;

	auto onStarted = [self]() {
		if (!self)
			return;
		self->m_capturing      = true;
		self->m_frameNumber    = 0;
		self->m_lastPreviewEmit = {};
	};
	auto onCancelled = [self]() {
		if (!self)
			return;
		emit self->captureAborted();
	};
	auto onError = [self](QString error) {
		if (!self)
			return;
		Global::get().l->log(Log::Warning, QObject::tr("Screen capture failed: %1").arg(error));
		self->m_capturing = false;
		self->destroyEncoder();
		emit self->captureAborted();
		// Bypasses stopCapture() for the same reason as the webcam error path above.
		emit self->captureStopped();
	};
	auto onFrame = [self](QImage frame) {
		if (!self || !self->m_capturing)
			return;
		self->encodeImage(frame);
	};

#		ifdef Q_OS_MAC
	sckit_startWithNativePicker(std::move(onStarted), std::move(onCancelled), std::move(onError), std::move(onFrame));
#		else
	xdg_portal_startCapture(std::move(onStarted), std::move(onCancelled), std::move(onError), std::move(onFrame));
#		endif
}
#	endif // Q_OS_MAC || HAS_WAYLAND_PORTAL

void ScreenCapture::encodeImage(const QImage &srcImage) {
	if (srcImage.isNull()) {
		scheduleCaptureAbort();
		return;
	}

	// Local self-preview tap: the source frame before any profile scaling or encoding. Runs on
	// the GUI thread (timer and native paths both funnel through here).
	if (previewFrameDue())
		emit previewFrame(srcImage);

	const Mumble::VideoQuality::Profile &profile = m_source.type == CaptureSource::Type::Webcam
														 ? Mumble::VideoQuality::webcamProfile()
														 : Mumble::VideoQuality::screenShareProfile();
	const QSize encodedSize                      = Mumble::VideoQuality::constrainedFrameSize(srcImage.size(), profile);
	if (!encodedSize.isValid()) {
		scheduleCaptureAbort();
		return;
	}

	QImage image = srcImage;
	if (image.size() != encodedSize)
		image = image.scaled(encodedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	// Convert to RGBA for mapping to AV_PIX_FMT_RGBA.
	image            = image.convertToFormat(QImage::Format_RGBA8888);
	const int width  = image.width();
	const int height = image.height();

	// (Re-)initialise the encoder when the resolution changes.
	if (!m_codecCtx || m_encoderWidth != width || m_encoderHeight != height) {
		destroyEncoder();
		if (!initEncoder(width, height, profile)) {
			scheduleCaptureAbort();
			return;
		}
	}

	// Colour-space conversion: RGBA24 to YUV420P.
	m_swsCtx = sws_getCachedContext(m_swsCtx, width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUV420P,
									SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!m_swsCtx) {
		scheduleCaptureAbort();
		return;
	}

	if (av_frame_make_writable(m_frame) < 0) {
		scheduleCaptureAbort();
		return;
	}

	const uint8_t *srcData[1] = { image.constBits() };
	int srcLinesize[1]        = { static_cast< int >(image.bytesPerLine()) };
	sws_scale(m_swsCtx, srcData, srcLinesize, 0, height, m_frame->data, m_frame->linesize);

	m_frame->pts = static_cast< int64_t >(m_frameNumber);

	if (avcodec_send_frame(m_codecCtx, m_frame) < 0) {
		scheduleCaptureAbort();
		return;
	}

	while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
		QByteArray encodedData(reinterpret_cast< const char * >(m_packet->data), m_packet->size);
		const bool isKey = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
		if (!m_reportedCaptureStarted) {
			m_reportedCaptureStarted = true;
			emit captureStarted();
		}
		emit frameEncoded(encodedData, m_frameNumber, static_cast< quint32 >(m_encoderWidth),
						  static_cast< quint32 >(m_encoderHeight), isKey);
		av_packet_unref(m_packet);
	}

	++m_frameNumber;
}

void ScreenCapture::encodeYuvFrame(int width, int height, const uint8_t *const data[4],
								   const int linesize[4]) {
	if (width <= 0 || height <= 0 || !data[0]) {
		scheduleCaptureAbort();
		return;
	}

	const Mumble::VideoQuality::Profile &profile = Mumble::VideoQuality::webcamProfile();
	const QSize encodedSize = Mumble::VideoQuality::constrainedFrameSize(QSize(width, height), profile);
	if (!encodedSize.isValid()) {
		scheduleCaptureAbort();
		return;
	}

	// Local self-preview tap (V4L2 worker thread). Throttle before converting so a dropped
	// frame costs nothing — a full YUV->RGBA conversion at capture rate would be pure waste.
	if (previewFrameDue()) {
		const QImage preview = convertPreviewFrame(width, height, data, linesize);
		if (!preview.isNull())
			emit previewFrame(preview);
	}

	// (Re-)initialise the encoder when the captured or constrained resolution changes.
	if (!m_codecCtx || m_encoderWidth != encodedSize.width() || m_encoderHeight != encodedSize.height()) {
		destroyEncoder();
		if (!initEncoder(encodedSize.width(), encodedSize.height(), profile)) {
			scheduleCaptureAbort();
			return;
		}
	}

	m_swsCtx = sws_getCachedContext(m_swsCtx, width, height, AV_PIX_FMT_YUV420P, encodedSize.width(),
									encodedSize.height(), AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!m_swsCtx) {
		scheduleCaptureAbort();
		return;
	}

	if (av_frame_make_writable(m_frame) < 0) {
		scheduleCaptureAbort();
		return;
	}

	// Plane copy into the encoder's frame (kept for buffer-lifecycle consistency with encodeImage).
	sws_scale(m_swsCtx, data, linesize, 0, height, m_frame->data, m_frame->linesize);

	m_frame->pts = static_cast< int64_t >(m_frameNumber);

	if (avcodec_send_frame(m_codecCtx, m_frame) < 0) {
		scheduleCaptureAbort();
		return;
	}

	while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
		QByteArray encodedData(reinterpret_cast< const char * >(m_packet->data), m_packet->size);
		const bool isKey = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
		if (!m_reportedCaptureStarted) {
			m_reportedCaptureStarted = true;
			emit captureStarted();
		}
		emit frameEncoded(encodedData, m_frameNumber, static_cast< quint32 >(m_encoderWidth),
						  static_cast< quint32 >(m_encoderHeight), isKey);
		av_packet_unref(m_packet);
	}

	++m_frameNumber;
}

bool ScreenCapture::previewFrameDue() {
	// steady_clock is immune to wall-clock adjustments, which would otherwise stall or flood
	// the preview. Called from whichever single thread feeds the encoder for this capture
	// (GUI for screen paths, the V4L2 worker for webcams) — never both for one instance,
	// because the source type is fixed for the lifetime of a capture.
	const auto now = std::chrono::steady_clock::now();
	if (m_lastPreviewEmit.time_since_epoch().count() != 0
		&& now - m_lastPreviewEmit < std::chrono::milliseconds(PREVIEW_FRAME_INTERVAL_MS)) {
		return false;
	}
	m_lastPreviewEmit = now;
	return true;
}

QImage ScreenCapture::convertPreviewFrame(int width, int height, const uint8_t *const data[4],
										 const int linesize[4]) {
	// Scale down (never up) to keep the conversion cheap and the shipped QImage small; sws
	// wants even dimensions on the destination.
	int dstW = qMin(width, PREVIEW_MAX_WIDTH) & ~1;
	int dstH = qRound(double(height) * dstW / double(width)) & ~1;
	if (dstW < 2 || dstH < 2)
		return {};

	m_previewSwsCtx = sws_getCachedContext(m_previewSwsCtx, width, height, AV_PIX_FMT_YUV420P, dstW, dstH,
										   AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!m_previewSwsCtx)
		return {};

	// Padded destination: swscale's SIMD tails can overshoot the last row for some
	// widths, which would corrupt the heap with a plain QImage (see PaddedImage.h).
	QImage img = allocatePaddedRGBAImage(dstW, dstH);
	if (img.isNull())
		return {};

	uint8_t *dstData[1] = { img.bits() };
	int dstStride[1]    = { static_cast< int >(img.bytesPerLine()) };
	sws_scale(m_previewSwsCtx, data, linesize, 0, height, dstData, dstStride);

	m_previewWidth  = dstW;
	m_previewHeight = dstH;
	return img;
}

void ScreenCapture::freePreviewScaler() {
	if (m_previewSwsCtx) {
		sws_freeContext(m_previewSwsCtx);
		m_previewSwsCtx = nullptr;
	}
	m_previewWidth  = 0;
	m_previewHeight = 0;
}

#endif // USE_SCREEN_SHARING

void ScreenCapture::captureFrame() {
#ifdef USE_SCREEN_SHARING
	// Delegate platform-specific grab to CaptureSourceLister.
	QImage image = grabCaptureSource(m_source);
	if (image.isNull()) {
		Global::get().l->log(Log::Warning, QObject::tr("Screen capture failed."));
		abortCapture();
		return;
	}

	// Ensure Format_RGB888 (24-bit RGB, no alpha) for AV_PIX_FMT_RGB24 mapping.
	encodeImage(image.convertToFormat(QImage::Format_RGB888));
#endif
}

#ifdef USE_SCREEN_SHARING
void ScreenCapture::scheduleCaptureAbort() {
	QMetaObject::invokeMethod(this, &ScreenCapture::abortCapture, Qt::QueuedConnection);
}

void ScreenCapture::abortCapture() {
	if (!m_capturing)
		return;

	stopCapture();
	emit captureAborted();
}

bool ScreenCapture::initEncoder(int width, int height, const Mumble::VideoQuality::Profile &profile) {
	// To use hardware-accelerated encoding (e.g. h264_videotoolbox on macOS,
	// h264_nvenc on NVIDIA), replace "libx264" with the appropriate encoder name
	// and add any codec-specific option calls below.
	const char *encoderName = "libx264";
	const AVCodec *codec    = avcodec_find_encoder_by_name(encoderName);
	if (!codec) {
		// I'm logging straight into the chatbox here so I can test things. But this probably should be a qWarning
		Global::get().l->log(Log::Warning,
							 QObject::tr("H.264 encoder (libx264) not available. "
										 "Ensure libx264 is installed and libavcodec was compiled with it."));
		return false;
	}

	m_codecCtx = avcodec_alloc_context3(codec);
	if (!m_codecCtx)
		return false;

	m_codecCtx->width          = width;
	m_codecCtx->height         = height;
	m_codecCtx->time_base      = { 1, profile.framesPerSecond };
	m_codecCtx->pix_fmt        = AV_PIX_FMT_YUV420P;
	m_codecCtx->bit_rate       = profile.bitRate;
	m_codecCtx->rc_max_rate    = profile.bitRate;
	m_codecCtx->rc_buffer_size = profile.bitRate;
	m_codecCtx->gop_size       = profile.keyFrameInterval;

	// Minimise encoding latency. These could maybe be settings?
	av_opt_set(m_codecCtx->priv_data, "preset", "superfast", 0);
	av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);

	if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_frame = av_frame_alloc();
	if (!m_frame) {
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_frame->format = AV_PIX_FMT_YUV420P;
	m_frame->width  = width;
	m_frame->height = height;
	if (av_frame_get_buffer(m_frame, 0) < 0) {
		av_frame_free(&m_frame);
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_packet = av_packet_alloc();
	if (!m_packet) {
		av_frame_free(&m_frame);
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_encoderWidth  = width;
	m_encoderHeight = height;
	return true;
}

void ScreenCapture::destroyEncoder() {
	if (m_swsCtx) {
		sws_freeContext(m_swsCtx);
		m_swsCtx = nullptr;
	}
	if (m_frame) {
		av_frame_free(&m_frame);
	}
	if (m_packet) {
		av_packet_free(&m_packet);
	}
	if (m_codecCtx) {
		avcodec_free_context(&m_codecCtx);
	}
	m_encoderWidth  = 0;
	m_encoderHeight = 0;
}
#endif
