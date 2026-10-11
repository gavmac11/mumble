# Copyright The Mumble Developers. All rights reserved.
# BSD-style license: see LICENSE at the source root.
add_executable(TestDatabasePath TestDatabasePath.cpp)
set_target_properties(TestDatabasePath PROPERTIES AUTOMOC ON)
target_include_directories(TestDatabasePath PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")
target_link_libraries(TestDatabasePath PRIVATE mumble_client_object_lib Qt6::Test)
add_test(NAME TestDatabasePath COMMAND $<TARGET_FILE:TestDatabasePath> -platform offscreen)
set_tests_properties(TestDatabasePath PROPERTIES TIMEOUT 30)
mumble_client_test_runtime(TestDatabasePath)
