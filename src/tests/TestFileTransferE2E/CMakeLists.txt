# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE/>.
#
# End-to-end file-transfer test: spawns a real murmur on a scratch port and
# runs two full Mumble client connections (TLS + protobuf protocol, no GUI)
# whose engines exchange a real transfer through the server's relay path.

add_executable(TestFileTransferE2E
	TestFileTransferE2E.cpp
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

set_target_properties(TestFileTransferE2E PROPERTIES AUTOMOC ON)

target_include_directories(TestFileTransferE2E PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/mumble"
	"${CMAKE_BINARY_DIR}/src"
)

find_pkg(Qt6 COMPONENTS Sql Network REQUIRED)

target_link_libraries(TestFileTransferE2E PRIVATE shared Qt6::Test Qt6::Sql Qt6::Network)
target_link_libraries(TestFileTransferE2E PRIVATE oqs argon2)

target_compile_definitions(TestFileTransferE2E PRIVATE
	MUMBLE_TEST_MURMUR_BINARY="$<TARGET_FILE:mumble-server>"
)

add_test(NAME TestFileTransferE2E COMMAND $<TARGET_FILE:TestFileTransferE2E>)
set_tests_properties(TestFileTransferE2E PROPERTIES TIMEOUT 600)
