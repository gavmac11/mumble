// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENCAPTURE_H_
#define MUMBLE_MUMBLE_SCREENCAPTURE_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <chrono>
#include <cstdint>

#ifdef USE_SCREEN_SHARING
#	include "CaptureSource.h"
#	include "VideoQualityProfile.h"
#	if defined(Q_OS_LINUX)
#		include "V4L2Capture.h"
#	endif
#	if defined(__APPLE__) && defined(__clang__)
#		pragma clang diagnostic push
#		pragma clang diagnostic ignored "-Wsign-conversion"
#		pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#	endif
extern "C" {
#	include <libavcodec/avcodec.h>
#	include <libavutil/opt.h>
#	include <libswscale/swscale.h>
}
#	if defined(__APPLE__) && defined(__clang__)
#		pragma clang diagnostic pop
#	endif
#endif

/// Captures screen content at up to 1080p15 or webcam content at up to 720p30 and emits
/// encoded video frames via frameEncoded().
///
/// On macOS 14+, startCaptureNative() shows the OS-native SCContentSharingPicker and streams
/// frames via SCStream; captureStarted() / captureAborted() signals report the outcome.
/// On Linux under Wayland, startCaptureNative() uses the xdg-desktop-portal ScreenCast interface
/// and delivers frames via a PipeWire stream.
/// On other platforms (or macOS < 14, or X11), use setSource() + startCapture() with ScreenPickerDialog.
///
/// Requires the build option -Dscreen-sharing=ON (links libavcodec/libswscale).
class ScreenCapture : public QObject {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ScreenCapture)

public:
	explicit ScreenCapture(QObject *parent = nullptr);
	~ScreenCapture() override;

	void startCapture();
	void stopCapture();
	bool isCapturing() const;

#ifdef USE_SCREEN_SHARING
	/// Sets the capture source for the non-native picker path. Call before startCapture().
	void setSource(const CaptureSource &source);

#	if defined(Q_OS_MAC) || defined(HAS_WAYLAND_PORTAL)
	/// Shows the platform-native picker and starts capturing asynchronously.
	/// On macOS 14+: uses SCContentSharingPicker / SCStream.
	/// On Linux (Wayland): uses xdg-desktop-portal ScreenCast + PipeWire.
	/// captureStarted() is emitted when frames begin; captureAborted() if cancelled/failed.
	void startCaptureNative();
#	endif
#endif

signals:
	/// Emitted for every successfully encoded frame.
	void frameEncoded(QByteArray encodedData, quint64 frameNumber, quint32 width, quint32 height, bool isKeyFrame);
	/// A source frame for the local "what am I sending" preview, throttled to a few frames per
	/// second. Emitted on the GUI thread for screen capture and on the V4L2 worker thread for
	/// webcams, so use an auto/queued connection when handing it to UI code.
	void previewFrame(QImage frame);
	/// Emitted whenever capture has actually stopped: user stop, disconnect, abort or capture
	/// failure. Emitted after any capture worker thread has been joined.
	void captureStopped();

#ifdef USE_SCREEN_SHARING
	/// Emitted immediately before the first successfully encoded frame.
	void captureStarted();
	/// Emitted when source selection is cancelled or capture/encoding fails.
	void captureAborted();
#endif

private slots:
	void captureFrame();
#ifdef USE_SCREEN_SHARING
	void abortCapture();
#endif

private:
#ifdef USE_SCREEN_SHARING
	bool initEncoder(int width, int height, const Mumble::VideoQuality::Profile &profile);
	void destroyEncoder();
	void scheduleCaptureAbort();
	void encodeImage(const QImage &srcImage); ///< Shared encode path used by both capture modes.
	/// Encode a planar YUV 4:2:0 frame supplied by the V4L2 webcam capture thread.
	void encodeYuvFrame(int width, int height, const uint8_t *const data[4], const int linesize[4]);
	/// Converts a planar YUV 4:2:0 frame to a scaled RGBA QImage for previewFrame(). Only ever
	/// called from the same thread as the encoders' frame input (worker for webcams, GUI otherwise).
	QImage convertPreviewFrame(int width, int height, const uint8_t *const data[4], const int linesize[4]);
	/// Throttle for previewFrame() emission; resets whenever a capture (re)starts.
	bool previewFrameDue();
	void freePreviewScaler();

	CaptureSource m_source; ///< Defaults to EntireScreen, screenIndex=0 (primary display).

	AVCodecContext *m_codecCtx = nullptr;
	AVFrame *m_frame           = nullptr;
	AVPacket *m_packet         = nullptr;
	SwsContext *m_swsCtx       = nullptr;
	int m_encoderWidth         = 0;
	int m_encoderHeight        = 0;
	bool m_reportedCaptureStarted = false;

	/// Preview scaler, deliberately independent of m_swsCtx: destroyEncoder() may free that one
	/// from the worker thread mid-stream, while the preview context is only ever freed from
	/// stopCapture() / the destructor, i.e. after the worker has been joined.
	SwsContext *m_previewSwsCtx = nullptr;
	int m_previewWidth          = 0;
	int m_previewHeight         = 0;
	/// Time of the last previewFrame() emission; default-constructed means "emit immediately".
	std::chrono::steady_clock::time_point m_lastPreviewEmit {};

#	if defined(Q_OS_LINUX)
	V4L2Capture *m_v4l2 = nullptr; ///< Webcam capture; worker thread invokes encodeYuvFrame().
#	endif
#endif

	QTimer *m_captureTimer = nullptr;
	quint64 m_frameNumber  = 0;
	bool m_capturing       = false;
};

#endif // MUMBLE_MUMBLE_SCREENCAPTURE_H_
