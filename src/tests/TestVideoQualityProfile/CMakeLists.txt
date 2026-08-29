# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestVideoQualityProfile
	TestVideoQualityProfile.cpp
	"${CMAKE_SOURCE_DIR}/src/mumble/VideoQualityProfile.cpp"
)

set_target_properties(TestVideoQualityProfile PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoQualityProfile PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")

target_link_libraries(TestVideoQualityProfile PRIVATE Qt6::Core Qt6::Test)

add_test(NAME TestVideoQualityProfile COMMAND $<TARGET_FILE:TestVideoQualityProfile>)
