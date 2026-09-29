# Hyperlink Host (Windows). Needs FFmpeg (shared build): set FFMPEG_DIR to a folder with
# include/, lib/ and bin/ (the CI downloads a BtbN build).
set(FFMPEG_DIR "" CACHE PATH "FFmpeg SDK folder (include, lib, bin)")
if(NOT FFMPEG_DIR)
    message(FATAL_ERROR "Set FFMPEG_DIR to an FFmpeg shared build")
endif()

enable_language(RC)
add_executable(HyperlinkHost WIN32
    src/main.cpp
    src/capture.cpp
    src/converter.cpp
    src/encoder.cpp
    src/input.cpp
    src/monitors.cpp
    src/server.cpp
    src/settings.cpp
    src/stream.cpp
    src/updater.cpp
    res/host.rc
)
target_include_directories(HyperlinkHost PRIVATE src "${FFMPEG_DIR}/include")
target_link_directories(HyperlinkHost PRIVATE "${FFMPEG_DIR}/lib")
target_compile_definitions(HyperlinkHost PRIVATE
    UNICODE _UNICODE
    HYPERLINK_VERSION="${PROJECT_VERSION}")
configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")
target_include_directories(HyperlinkHost PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(HyperlinkHost PRIVATE
    hyperlink_core avcodec avutil
    d3d11 dxgi avrt winmm winhttp bcrypt comctl32 shell32 user32 gdi32 ole32 shcore)
if(MSVC)
    target_compile_options(HyperlinkHost PRIVATE /W3)
    target_link_options(HyperlinkHost PRIVATE /MANIFEST:NO)
endif()

# Put the FFmpeg DLLs next to the exe.
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET HyperlinkHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:HyperlinkHost>)
