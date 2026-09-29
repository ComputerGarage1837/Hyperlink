cmake_minimum_required(VERSION 3.22)
project(hyperlink_client CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The shared core lives at the repository root.
set(HL_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)
add_subdirectory(${HL_CORE} ${CMAKE_BINARY_DIR}/core)

add_library(hyperlink SHARED jni.cpp decoder.cpp host_jni.cpp)
target_compile_options(hyperlink PRIVATE -O2 -Wall)
target_compile_options(hyperlink_core PRIVATE -O2)
target_link_libraries(hyperlink PRIVATE hyperlink_core mediandk android log)
