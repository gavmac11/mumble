# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestFileTransferEngine
	TestFileTransferEngine.cpp
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/Argon2Wrap.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/CanonicalCBOR.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/CryptoUtils.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/KemMLKEM768.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/Merkle.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/crypto/SigMLDSA65.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/identity/FTIdentity.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/identity/PeerTrustStore.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/engine/FTMessages.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/engine/FileTransferSession.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/engine/FTManifest.cpp"
	"${CMAKE_SOURCE_DIR}/src/mumble/PQFileTransfer/engine/FileTransferEngine.cpp"
)

set_target_properties(TestFileTransferEngine PROPERTIES AUTOMOC ON)

target_include_directories(TestFileTransferEngine PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/mumble"
	"${CMAKE_BINARY_DIR}/src"
)

find_pkg(Qt6 COMPONENTS Sql REQUIRED)

target_link_libraries(TestFileTransferEngine PRIVATE shared Qt6::Test Qt6::Sql)
target_link_libraries(TestFileTransferEngine PRIVATE oqs argon2)

add_test(NAME TestFileTransferEngine COMMAND $<TARGET_FILE:TestFileTransferEngine>)
set_tests_properties(TestFileTransferEngine PROPERTIES TIMEOUT 600)
