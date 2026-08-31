// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CAPTURESOURCE_H_
#define MUMBLE_MUMBLE_CAPTURESOURCE_H_

#ifdef USE_SCREEN_SHARING

#	include <QtCore/QString>
#	include <QtCore/QtGlobal>
#	include <QtGui/QPixmap>

/// Identifies a single capture source — an entire screen/monitor, a specific window, or a webcam.
struct CaptureSource {
	enum class Type { EntireScreen, Window, Webcam, NativePicker };

	Type type               = Type::EntireScreen;
	int screenIndex         = 0; ///< Index into QGuiApplication::screens() — used when type == EntireScreen.
	quintptr nativeWindowId = 0; ///< Platform window handle: CGWindowID on macOS, XID on X11, HWND on Windows.
	QString devicePath;         ///< Camera identifier: V4L2 device node on Linux, AVCaptureDevice unique ID on macOS.

	QString displayName; ///< Human-readable label shown in the picker (e.g. "Display 1 (2560×1440)").
	QPixmap thumbnail;   ///< Scaled preview (≈160×90), populated by listCaptureSources().
};

#endif // USE_SCREEN_SHARING
#endif // MUMBLE_MUMBLE_CAPTURESOURCE_H_
