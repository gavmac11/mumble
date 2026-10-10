# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestChatWebM TestChatWebM.cpp)

set_target_properties(TestChatWebM PROPERTIES AUTOMOC ON)

target_include_directories(TestChatWebM PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")

target_link_libraries(TestChatWebM PRIVATE mumble_client_object_lib Qt6::Test)

add_test(NAME TestChatWebM COMMAND $<TARGET_FILE:TestChatWebM>)
set_tests_properties(TestChatWebM PROPERTIES TIMEOUT 60)

mumble_client_test_runtime(TestChatWebM)
