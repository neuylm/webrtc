cmake_minimum_required(VERSION 3.13)

add_executable(my-client main.cpp)

set_target_properties(my-client PROPERTIES
    VERSION ${PROJECT_VERSION}
    CXX_STANDARD 17
    OUTPUT_NAME my-client)

target_include_directories(my-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_link_libraries(my-client
    PRIVATE
    mywebrtc
    Threads::Threads)

if(WIN32)
    target_link_libraries(my-client PRIVATE ws2_32)
endif()

if(NOT MSVC)
    target_compile_options(my-client PRIVATE -Wall -Wextra)
endif()
