# Copyright The Mumble Developers. All rights reserved.
# BSD-style license: see LICENSE at the source root.
add_executable(TestAudioWizard TestAudioWizard.cpp)
set_target_properties(TestAudioWizard PROPERTIES AUTOMOC ON)
target_include_directories(TestAudioWizard PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")
target_link_libraries(TestAudioWizard PRIVATE mumble_client_object_lib Qt6::Test)
add_test(NAME TestAudioWizard COMMAND $<TARGET_FILE:TestAudioWizard> -platform offscreen)
set_tests_properties(TestAudioWizard PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
mumble_client_test_runtime(TestAudioWizard)
