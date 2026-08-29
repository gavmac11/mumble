# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestVideoFramePacketizer
	TestVideoFramePacketizer.cpp
	"${CMAKE_SOURCE_DIR}/src/mumble/VideoFramePacketizer.cpp"
)

set_target_properties(TestVideoFramePacketizer PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoFramePacketizer PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/mumble"
	"${CMAKE_BINARY_DIR}/src"
)

target_link_libraries(TestVideoFramePacketizer PRIVATE shared Qt6::Test)

add_test(NAME TestVideoFramePacketizer COMMAND $<TARGET_FILE:TestVideoFramePacketizer>)
