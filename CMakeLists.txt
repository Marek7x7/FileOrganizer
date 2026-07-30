cmake_minimum_required(VERSION 3.15)
project(FileOrganizer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(organizer main.cpp)
target_link_libraries(organizer PRIVATE Threads::Threads)

option(ORGANIZER_BUILD_TESTS "Build the unit test suite" ON)
if(ORGANIZER_BUILD_TESTS)
    enable_testing()
    add_executable(organizer_tests tests/test_main.cpp tests/test_core.cpp)
    target_include_directories(organizer_tests PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME organizer_tests COMMAND organizer_tests)
endif()
