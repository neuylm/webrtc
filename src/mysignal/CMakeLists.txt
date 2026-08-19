cmake_minimum_required(VERSION 3.16)

set(MYSIGNAL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	set(MYSIGNAL_STANDALONE ON)
	project(mysignal LANGUAGES CXX)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	enable_testing()
else()
	set(MYSIGNAL_STANDALONE OFF)
endif()

include(GNUInstallDirs)

set(MYSIGNAL_SOURCES
	base64.cpp
	frame.cpp
	handshake.cpp
	jsonpick.cpp
	net.cpp
	sha1.cpp
	signal.cpp
	stunsrv.cpp)

add_library(mysignal STATIC ${MYSIGNAL_SOURCES})
set_target_properties(mysignal PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(mysignal
	PUBLIC ${MYSIGNAL_ROOT}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mysignal PUBLIC Threads::Threads)

if(WIN32)
	target_link_libraries(mysignal PUBLIC ws2_32)
endif()

if(MSVC)
	target_compile_options(mysignal PRIVATE /W4)
else()
	target_compile_options(mysignal PRIVATE -Wall -Wextra)
endif()

add_library(MySignal::MySignal ALIAS mysignal)

if(NOT NO_EXAMPLES)
	add_executable(signal-server ${MYSIGNAL_ROOT}/examples/signal-server/main.cpp)
	set_target_properties(signal-server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(signal-server PRIVATE mysignal)
endif()

if(NOT NO_TESTS)
	add_executable(mysignal-test ${MYSIGNAL_ROOT}/test/mysignal/signal_test.cpp)
	set_target_properties(mysignal-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_include_directories(mysignal-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(mysignal-test PRIVATE mysignal)
	if(MYSIGNAL_STANDALONE)
		add_test(NAME mysignal COMMAND mysignal-test)
	endif()
endif()

install(TARGETS mysignal ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${MYSIGNAL_ROOT}/include/mysignal/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mysignal)
