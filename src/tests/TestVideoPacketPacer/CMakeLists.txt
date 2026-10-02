add_executable(TestVideoPacketPacer
	TestVideoPacketPacer.cpp
	"${CMAKE_SOURCE_DIR}/src/mumble/VideoPacketPacer.cpp"
)
set_target_properties(TestVideoPacketPacer PROPERTIES AUTOMOC ON)
target_include_directories(TestVideoPacketPacer PRIVATE "${CMAKE_SOURCE_DIR}/src/mumble")
target_link_libraries(TestVideoPacketPacer PRIVATE Qt6::Core Qt6::Test)
add_test(NAME TestVideoPacketPacer COMMAND $<TARGET_FILE:TestVideoPacketPacer>)
