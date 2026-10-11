// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_LOGGER_H_
#define MUMBLE_LOGGER_H_

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

class QTextEdit;

namespace mumble {
namespace log {
	constexpr const char *MainLoggerName = "Main";

	void init(spdlog::level::level_enum logLevel = spdlog::level::trace);
	// Call before Qt/application teardown while the logging registry is still alive.
	void restoreQtMessageHandler();
	void addSink(std::shared_ptr< spdlog::sinks::sink > sink);

	template< typename... Args > static void inline trace(spdlog::format_string_t< Args... > fmt, Args &&... args) {
		spdlog::trace(fmt, std::forward< Args >(args)...);
	}

	template< typename... Args > static void inline debug(spdlog::format_string_t< Args... > fmt, Args &&... args) {
		spdlog::debug(fmt, std::forward< Args >(args)...);
	}

	template< typename... Args > static void inline info(spdlog::format_string_t< Args... > fmt, Args &&... args) {
		spdlog::info(fmt, std::forward< Args >(args)...);
	}

	template< typename... Args > static void inline warn(spdlog::format_string_t< Args... > fmt, Args &&... args) {
		spdlog::warn(fmt, std::forward< Args >(args)...);
	}

	template< typename... Args > static void inline error(spdlog::format_string_t< Args... > fmt, Args &&... args) {
		spdlog::error(fmt, std::forward< Args >(args)...);
	}

	template< typename... Args > static void inline fatal(spdlog::format_string_t< Args... > fmt, Args &&...args) {
		spdlog::critical(fmt, std::forward< Args >(args)...);
		if (auto logger = spdlog::default_logger())
			logger->flush();
		// Fatal calls do not unwind main's handler-restoration guard. Static
		// teardown can therefore log through a destroyed registry; skip it.
		std::_Exit(1);
	}
} // namespace log
} // namespace mumble

#endif
