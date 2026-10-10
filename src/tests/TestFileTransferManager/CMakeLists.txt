# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestFileTransferManager TestFileTransferManager.cpp)
set_target_properties(TestFileTransferManager PROPERTIES AUTOMOC ON)
target_include_directories(TestFileTransferManager PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")
target_link_libraries(TestFileTransferManager PRIVATE mumble_client_object_lib Qt6::Test)
add_test(NAME TestFileTransferManager COMMAND $<TARGET_FILE:TestFileTransferManager>)
set_tests_properties(TestFileTransferManager PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
