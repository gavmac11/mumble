// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PADDEDIMAGE_H_
#define MUMBLE_MUMBLE_PADDEDIMAGE_H_

#include <QtGui/QImage>

#include <libavutil/mem.h>

/// Allocates an RGBA image that is safe to use as a sws_scale destination.
///
/// swscale's SIMD tail handling can write past the end of the last row for some
/// widths (observed with FFmpeg 8 on AVX2: a width of 1660 px, i.e. width % 16 == 12,
/// writes 16 bytes beyond stride * height). A QImage constructed with (width, height)
/// allocates exactly width * height * 4 bytes with a stride that is only 4-byte
/// aligned, so such an overshoot corrupts the heap. This helper allocates the storage
/// itself instead: rows are padded to a 64-byte stride and 64 bytes of slack follow
/// the final row, absorbing any SIMD overshoot. The QImage frees the buffer via its
/// cleanup hook once its last copy has been destroyed, so it can be handed across
/// threads like any implicitly shared QImage.
inline QImage allocatePaddedRGBAImage(int width, int height) {
	if (width <= 0 || height <= 0)
		return QImage();

	const qsizetype stride = (static_cast< qsizetype >(width) * 4 + 63) & ~static_cast< qsizetype >(63);
	const qsizetype bytes  = stride * height + 64;

	uchar *data = static_cast< uchar * >(av_malloc(static_cast< size_t >(bytes)));
	if (!data)
		return QImage();

	return QImage(data, width, height, stride, QImage::Format_RGBA8888,
				  [](void *buffer) { av_free(buffer); }, data);
}

#endif // MUMBLE_MUMBLE_PADDEDIMAGE_H_
