cmake_minimum_required(VERSION 3.16)

set(MYAUDIO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	set(MYAUDIO_STANDALONE ON)
	project(myaudio LANGUAGES CXX)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	enable_testing()
else()
	set(MYAUDIO_STANDALONE OFF)
endif()

include(GNUInstallDirs)

if(NOT TARGET myrtm)
	set(MYAUDIO_KEEP_EXAMPLES ${NO_EXAMPLES})
	set(MYAUDIO_KEEP_TESTS ${NO_TESTS})
	set(NO_EXAMPLES ON)
	set(NO_TESTS ON)
	add_subdirectory(${MYAUDIO_ROOT}/src/myrtm ${CMAKE_CURRENT_BINARY_DIR}/myrtm)
	set(NO_EXAMPLES ${MYAUDIO_KEEP_EXAMPLES})
	set(NO_TESTS ${MYAUDIO_KEEP_TESTS})
endif()

set(MYAUDIO_SOURCES
	decode.cpp
	device.cpp
	mic.cpp
	mixer.cpp
	resample.cpp
	share.cpp
	sink_basic.cpp
	sink_wasapi.cpp
	source.cpp)

add_library(myaudio STATIC ${MYAUDIO_SOURCES})
set_target_properties(myaudio PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(myaudio
	PUBLIC ${MYAUDIO_ROOT}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(myaudio PUBLIC myrtm Threads::Threads)

find_package(Opus CONFIG QUIET)
if(TARGET Opus::opus)
	target_link_libraries(myaudio PRIVATE Opus::opus)
	target_compile_definitions(myaudio PRIVATE MYAUDIO_HAS_OPUS)
	set(MYAUDIO_OPUS ON)
else()
	find_path(OPUS_INCLUDE_DIR opus/opus.h)
	find_library(OPUS_LIBRARY NAMES opus)
	if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
		target_include_directories(myaudio PRIVATE ${OPUS_INCLUDE_DIR})
		target_link_libraries(myaudio PRIVATE ${OPUS_LIBRARY})
		target_compile_definitions(myaudio PRIVATE MYAUDIO_HAS_OPUS)
		set(MYAUDIO_OPUS ON)
	else()
		set(MYAUDIO_OPUS OFF)
		message(STATUS "myaudio: libopus not found, Opus frames will be rejected")
	endif()
endif()

if(WIN32)
	target_link_libraries(myaudio PUBLIC ole32 uuid avrt advapi32)
endif()

if(MSVC)
	target_compile_options(myaudio PRIVATE /W4)
else()
	target_compile_options(myaudio PRIVATE -Wall -Wextra)
endif()

add_library(MyAudio::MyAudio ALIAS myaudio)

if(NOT NO_EXAMPLES)
	add_executable(mic-inject ${MYAUDIO_ROOT}/examples/mic-inject/main.cpp)
	set_target_properties(mic-inject PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(mic-inject PRIVATE myaudio)
endif()

if(NOT NO_TESTS)
	add_executable(myaudio-test ${MYAUDIO_ROOT}/test/myaudio/audio_test.cpp)
	set_target_properties(myaudio-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_include_directories(myaudio-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(myaudio-test PRIVATE myaudio)
	if(MYAUDIO_OPUS)
		target_compile_definitions(myaudio-test PRIVATE MYAUDIO_HAS_OPUS)
		if(TARGET Opus::opus)
			target_link_libraries(myaudio-test PRIVATE Opus::opus)
		else()
			target_include_directories(myaudio-test PRIVATE ${OPUS_INCLUDE_DIR})
			target_link_libraries(myaudio-test PRIVATE ${OPUS_LIBRARY})
		endif()
	endif()
	if(MYAUDIO_STANDALONE)
		add_test(NAME myaudio COMMAND myaudio-test)
	endif()
endif()

install(TARGETS myaudio ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${MYAUDIO_ROOT}/include/myaudio/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/myaudio)
