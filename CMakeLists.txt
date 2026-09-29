cmake_minimum_required(VERSION 3.20)
# Static MSVC runtime so the Windows builds don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HL_VERSION)
string(STRIP "${HL_VERSION}" HL_VERSION)
project(Hyperlink VERSION ${HL_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(core)

option(HYPERLINK_TESTS "Build the core unit tests" ON)
if(HYPERLINK_TESTS)
    enable_testing()
    add_executable(core_tests core/tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE hyperlink_core)
    add_test(NAME core_tests COMMAND core_tests)
endif()

if(WIN32)
    add_subdirectory(host)
endif()
