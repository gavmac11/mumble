# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestConnectionOutputQueue
	TestConnectionOutputQueue.cpp
	"${CMAKE_SOURCE_DIR}/src/Connection.cpp"
	"${CMAKE_SOURCE_DIR}/src/Connection.h"
)
set_target_properties(TestConnectionOutputQueue PROPERTIES AUTOMOC ON)
target_link_libraries(TestConnectionOutputQueue PRIVATE shared Qt6::Test)
add_test(NAME TestConnectionOutputQueue COMMAND $<TARGET_FILE:TestConnectionOutputQueue>)
set_tests_properties(TestConnectionOutputQueue PROPERTIES TIMEOUT 30)
