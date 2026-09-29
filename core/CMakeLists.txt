add_library(hyperlink_core STATIC
    src/client.cpp
    src/common.cpp
    src/fec.cpp
    src/json.cpp
    src/net.cpp
    src/protocol.cpp
    src/video.cpp
)
target_include_directories(hyperlink_core PUBLIC include)
target_compile_features(hyperlink_core PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(hyperlink_core PUBLIC ws2_32 iphlpapi)
endif()
if(WIN32)
    target_compile_definitions(hyperlink_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()
if(MSVC)
    target_compile_options(hyperlink_core PUBLIC /utf-8)
endif()
