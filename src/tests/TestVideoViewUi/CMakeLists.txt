# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestVideoViewUi TestVideoViewUi.cpp)
set_target_properties(TestVideoViewUi PROPERTIES AUTOMOC ON)
target_include_directories(TestVideoViewUi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../mumble")
target_link_libraries(TestVideoViewUi PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME TestVideoViewUi COMMAND $<TARGET_FILE:TestVideoViewUi> -platform offscreen)
