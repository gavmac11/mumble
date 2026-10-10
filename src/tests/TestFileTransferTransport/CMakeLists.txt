# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

cmake_minimum_required(VERSION 3.23)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(TestFileTransferTransport LANGUAGES CXX)
	set(CMAKE_CXX_STANDARD 20)
	find_package(Qt6 REQUIRED COMPONENTS Core Test)
	enable_testing()
endif()

add_executable(TestFileTransferTransport TestFileTransferTransport.cpp)
set_target_properties(TestFileTransferTransport PROPERTIES AUTOMOC ON)
target_include_directories(TestFileTransferTransport PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../mumble")
target_link_libraries(TestFileTransferTransport PRIVATE Qt6::Core Qt6::Test)
add_test(NAME TestFileTransferTransport COMMAND $<TARGET_FILE:TestFileTransferTransport>)
set_tests_properties(TestFileTransferTransport PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_FATAL_WARNINGS=1")
