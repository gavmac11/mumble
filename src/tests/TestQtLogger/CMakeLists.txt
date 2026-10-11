# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license.
add_executable(TestQtLogger TestQtLogger.cpp)
set_target_properties(TestQtLogger PROPERTIES AUTOMOC ON)
target_link_libraries(TestQtLogger PRIVATE shared Qt6::Test)
add_test(NAME TestQtLogger COMMAND $<TARGET_FILE:TestQtLogger>)
set_tests_properties(TestQtLogger PROPERTIES TIMEOUT 30)
