# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestIPCUtils TestIPCUtils.cpp)

set_target_properties(TestIPCUtils PROPERTIES AUTOMOC ON)

# MSVC's implicit _WIN32 macro is not visible to moc. Supply the same
# explicit test platform to both the compiler and metaobject generator.
if(WIN32)
	target_compile_definitions(TestIPCUtils PRIVATE MUMBLE_TEST_WINDOWS)
elseif(APPLE)
	target_compile_definitions(TestIPCUtils PRIVATE MUMBLE_TEST_MACOS)
endif()

target_link_libraries(TestIPCUtils PRIVATE ipcutils Qt6::Test)

add_test(NAME TestIPCUtils COMMAND $<TARGET_FILE:TestIPCUtils>)
