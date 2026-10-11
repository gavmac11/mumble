# Copyright The Mumble Developers. All rights reserved.
# BSD-style license: see LICENSE at the source root.
add_executable(TestFileTransferUi TestFileTransferUi.cpp)
set_target_properties(TestFileTransferUi PROPERTIES AUTOMOC ON)
target_include_directories(TestFileTransferUi PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")
target_link_libraries(TestFileTransferUi PRIVATE mumble_client_object_lib Qt6::Test)
add_test(NAME TestFileTransferUi COMMAND $<TARGET_FILE:TestFileTransferUi> -platform offscreen)
set_tests_properties(TestFileTransferUi PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
set_property(TEST TestFileTransferUi APPEND PROPERTY ENVIRONMENT
	"MUMBLE_QR_FIXTURE_EXPORT=${CMAKE_BINARY_DIR}/test-results/safety-qr")
mumble_client_test_runtime(TestFileTransferUi)
