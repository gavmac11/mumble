# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestAnimatedImages TestAnimatedImages.cpp)

set_target_properties(TestAnimatedImages PROPERTIES AUTOMOC ON)

target_include_directories(TestAnimatedImages PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")

target_link_libraries(TestAnimatedImages PRIVATE mumble_client_object_lib Qt6::Test)

add_test(NAME TestAnimatedImages COMMAND $<TARGET_FILE:TestAnimatedImages>)
set_tests_properties(TestAnimatedImages PROPERTIES TIMEOUT 60)
