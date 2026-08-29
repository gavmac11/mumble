// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file at the root of the Mumble source tree or at
// <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_V4L2CAPTURE_H_
#define MUMBLE_MUMBLE_V4L2CAPTURE_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <functional>

#ifdef USE_SCREEN_SHARING

/// Opens a V4L2 webcam via FFmpeg's video4linux2 input on a dedicated worker thread and delivers
/// decoded, scale-converted YUV420P frames. Mirrors the callback style of XdgPortalCapture:
/// all callbacks are invoked on the capture thread, not the GUI thread.
///
/// The frame callback receives the frame's dimensions plus its plane pointers and strides
/// (data[0..3] / linesize[0..3] in FFmpeg convention). The pointers are only valid for the
/// duration of the call — copy or convert before returning.
class V4L2Capture {
public:
	/// Called once the first frame has been decoded (capture thread).
	using StartedCallback  = std::function< void() >;
	/// Called on cancellation or failure with a human-readable reason (capture thread).
	using ErrorCallback    = std::function< void(QString) >;
	/// Called for every decoded frame, converted to planar YUV 4:2:0 (capture thread).
	using YuvFrameCallback = std::function< void(int width, int height, const uint8_t *const data[4],
												 const int linesize[4]) >;

	~V4L2Capture();

	/// Spawns the capture thread. Must not be called while already running.
	void start(const QString &devicePath, StartedCallback onStarted, ErrorCallback onError,
			   YuvFrameCallback onYuvFrame);

	/// Stops the capture thread and joins it. Safe to call from any thread; never blocks
	/// indefinitely thanks to the FFmpeg interrupt callback.
	void stop();

	bool isRunning() const { return m_running; }

private:
	void run(const QString &devicePath, StartedCallback onStarted, ErrorCallback onError,
			 YuvFrameCallback onYuvFrame);

	struct Impl;
	Impl *m_impl      = nullptr;
	bool m_running    = false;
};

#endif // USE_SCREEN_SHARING
#endif // MUMBLE_MUMBLE_V4L2CAPTURE_H_
