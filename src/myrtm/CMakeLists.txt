cmake_minimum_required(VERSION 3.16)

set(MYRTM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	set(MYRTM_STANDALONE ON)
	project(myrtm LANGUAGES CXX)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	enable_testing()
else()
	set(MYRTM_STANDALONE OFF)
endif()

include(GNUInstallDirs)

set(MYRTM_SOURCES
	aes.cpp
	channel.cpp
	codec.cpp
	crypto.cpp
	endpoint.cpp
	session.cpp
	socket.cpp
	stun.cpp
	wire.cpp)

add_library(myrtm STATIC ${MYRTM_SOURCES})
set_target_properties(myrtm PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(myrtm
	PUBLIC ${MYRTM_ROOT}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(myrtm PUBLIC Threads::Threads)

if(WIN32)
	target_link_libraries(myrtm PUBLIC ws2_32 bcrypt)
endif()

if(MSVC)
	target_compile_options(myrtm PRIVATE /W4)
else()
	target_compile_options(myrtm PRIVATE -Wall -Wextra)
endif()

add_library(MyRtm::MyRtm ALIAS myrtm)

if(NOT NO_EXAMPLES)
	add_executable(rtm-demo ${MYRTM_ROOT}/examples/rtm-demo/main.cpp)
	set_target_properties(rtm-demo PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(rtm-demo PRIVATE myrtm)
endif()

if(NOT NO_TESTS)
	add_executable(myrtm-test ${MYRTM_ROOT}/test/myrtm/rtm_test.cpp)
	set_target_properties(myrtm-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_include_directories(myrtm-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(myrtm-test PRIVATE myrtm)
	if(MYRTM_STANDALONE)
		add_test(NAME myrtm COMMAND myrtm-test)
	endif()
endif()

install(TARGETS myrtm ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${MYRTM_ROOT}/include/myrtm/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/myrtm)
