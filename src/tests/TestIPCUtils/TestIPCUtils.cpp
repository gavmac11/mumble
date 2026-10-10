// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "IPCUtils.h"

#include <QByteArray>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>

#ifndef MUMBLE_TEST_WINDOWS
#	include <cstdlib>
#endif

#ifdef MUMBLE_TEST_MACOS
#	include <unistd.h>
#endif

namespace {

#ifndef MUMBLE_TEST_WINDOWS
class EnvironmentVariableGuard {
public:
	explicit EnvironmentVariableGuard(const char *name)
		: m_name(name), m_originalValue(std::getenv(name)), m_wasSet(m_originalValue != nullptr) {}

	~EnvironmentVariableGuard() {
		if (m_wasSet) {
			setenv(m_name, m_originalValue.constData(), 1);
		} else {
			unsetenv(m_name);
		}
	}

private:
	const char *m_name;
	QByteArray m_originalValue;
	bool m_wasSet;
};
#endif

#ifdef MUMBLE_TEST_MACOS
std::filesystem::path darwinUserTemporaryDirectory() {
	const std::size_t pathLength = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
	if (pathLength == 0) {
		return {};
	}

	QByteArray path(static_cast< int >(pathLength), '\0');
	if (confstr(_CS_DARWIN_USER_TEMP_DIR, path.data(), pathLength) == 0) {
		return {};
	}

	return path.constData();
}
#endif

} // namespace

class TestIPCUtils : public QObject {
	Q_OBJECT
private slots:

#ifndef MUMBLE_TEST_WINDOWS
	void usesXdgRuntimeDirectoryWhenSet() {
		QTemporaryDir temporaryDirectory;
		QVERIFY(temporaryDirectory.isValid());

		EnvironmentVariableGuard xdgRuntimeDirGuard("XDG_RUNTIME_DIR");
		QVERIFY(setenv("XDG_RUNTIME_DIR", temporaryDirectory.path().toUtf8().constData(), 1) == 0);

		const std::filesystem::path runtimeDirectory = Mumble::getRuntimeDirectory();

		QVERIFY(runtimeDirectory.parent_path() == std::filesystem::path(temporaryDirectory.path().toStdString()));
		QVERIFY(runtimeDirectory.filename() == std::filesystem::path("info.mumble.Mumble"));
		QVERIFY(std::filesystem::is_directory(runtimeDirectory));
	}
#endif

#ifdef MUMBLE_TEST_MACOS
	void usesDarwinUserTemporaryDirectoryWithoutXdgRuntimeDir() {
		EnvironmentVariableGuard xdgRuntimeDirGuard("XDG_RUNTIME_DIR");
		unsetenv("XDG_RUNTIME_DIR");

		const std::filesystem::path expectedParent = darwinUserTemporaryDirectory();
		QVERIFY2(!expectedParent.empty(), "Unable to determine the Darwin user temporary directory");

		const std::filesystem::path runtimeDirectory = Mumble::getRuntimeDirectory();

		QVERIFY(std::filesystem::weakly_canonical(runtimeDirectory.parent_path())
				== std::filesystem::weakly_canonical(expectedParent));
		QVERIFY(runtimeDirectory.filename() == std::filesystem::path("info.mumble.Mumble"));
		QVERIFY(std::filesystem::is_directory(runtimeDirectory));
	}
#endif

#ifdef MUMBLE_TEST_WINDOWS
	void windowsUsesNativePipeNames() {
		QVERIFY(Mumble::getRuntimeDirectory().empty());
		QVERIFY(Mumble::getOverlayPipePath() == std::filesystem::path("MumbleOverlayPipe"));
		QVERIFY(Mumble::getOverlayPipeDevicePath() == LR"(\\.\pipe\MumbleOverlayPipe)");
		QVERIFY(Mumble::getSocketPath("connection") == std::filesystem::path("connection"));
	}
#endif
};

QTEST_MAIN(TestIPCUtils)
#include "TestIPCUtils.moc"
