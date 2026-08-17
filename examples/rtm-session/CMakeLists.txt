cmake_minimum_required(VERSION 3.13)

add_executable(rtm-session main.cpp)

set_target_properties(rtm-session PROPERTIES
    VERSION ${PROJECT_VERSION}
    CXX_STANDARD 17
    OUTPUT_NAME rtm-session)

target_include_directories(rtm-session PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_link_libraries(rtm-session
    PRIVATE
    mywebrtc
    myrtm
    Threads::Threads)

if(WIN32)
    target_link_libraries(rtm-session PRIVATE ws2_32)
endif()

if(NOT MSVC)
    target_compile_options(rtm-session PRIVATE -Wall -Wextra)
endif()
