add_executable(TestBandwidthRecord
	TestBandwidthRecord.cpp
	"${CMAKE_SOURCE_DIR}/src/murmur/BandwidthRecord.cpp"
	"${CMAKE_SOURCE_DIR}/src/Timer.cpp"
)
set_target_properties(TestBandwidthRecord PROPERTIES AUTOMOC ON)
target_include_directories(TestBandwidthRecord PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/murmur"
)
target_link_libraries(TestBandwidthRecord PRIVATE Qt6::Core Qt6::Test)
add_test(NAME TestBandwidthRecord COMMAND $<TARGET_FILE:TestBandwidthRecord>)
