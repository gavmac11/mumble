# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestPQCrypto
	TestPQCrypto.cpp
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/Argon2Wrap.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/CanonicalCBOR.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/CryptoUtils.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/KemMLKEM768.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/Merkle.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/SigMLDSA65.cpp"
)

set_target_properties(TestPQCrypto PROPERTIES AUTOMOC ON)

target_include_directories(TestPQCrypto PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/mumble"
	"${CMAKE_BINARY_DIR}/src"
)

target_link_libraries(TestPQCrypto PRIVATE shared Qt6::Test)
target_link_libraries(TestPQCrypto PRIVATE oqs argon2)

add_test(NAME TestPQCrypto COMMAND $<TARGET_FILE:TestPQCrypto>)
set_tests_properties(TestPQCrypto PROPERTIES TIMEOUT 300)
