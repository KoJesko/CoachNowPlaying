cmake_minimum_required(VERSION 3.20)
project(CoachNowPlaying LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the plugin must not need any redistributable on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# Windows builds read the Windows media controls (media.cpp, WinRT). The Linux/Proton build (mingw / llvm-mingw)
# asks the native helper linux/coach-mpris-bridge.py instead, because Wine has no media controls.
if(MSVC)
    set(CNP_MEDIA_SOURCE src/media.cpp)
else()
    set(CNP_MEDIA_SOURCE src/media_mpris.cpp)
endif()

add_library(CoachNowPlaying SHARED
    src/plugin.cpp
    src/log.cpp
    src/sigscan.cpp
    src/safe_mode.cpp
    src/engine_hooks.cpp
    src/screen_assets.cpp
    src/d3d11_hooks.cpp
    src/touch.cpp
    src/screen_fit.cpp
    src/net_hooks.cpp
    src/radio_tracker.cpp
    src/extra_sources.cpp
    src/station_db.cpp
    ${CNP_MEDIA_SOURCE}
    src/settings.cpp
    src/text_util.cpp
    src/http.cpp
    src/cover_lookup.cpp
    src/station_logo.cpp
    src/renderer.cpp
    src/eta_screen.cpp
    src/passenger.cpp
    src/boot_anim.cpp
    src/plate.cpp
    src/exports.def)
set_target_properties(CoachNowPlaying PROPERTIES PREFIX "")
target_include_directories(CoachNowPlaying PRIVATE src third_party/scssdk/include)
target_compile_definitions(CoachNowPlaying PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(CoachNowPlaying PRIVATE /utf-8 /EHsc /permissive- /W3)
    target_link_libraries(CoachNowPlaying PRIVATE
        minhook d3d11 dxgi d2d1 dwrite windowscodecs winhttp ws2_32 windowsapp shell32 ole32 gdi32 winmm sapi)
else()
    target_compile_options(CoachNowPlaying PRIVATE -finput-charset=UTF-8 -fexceptions -fms-extensions -Wall -Wno-unused-function)
    # Fully static runtime: the game folder has no libc++/libunwind/winpthread DLLs.
    target_link_options(CoachNowPlaying PRIVATE -static -static-libgcc -static-libstdc++ -s)
    target_link_libraries(CoachNowPlaying PRIVATE
        minhook d3d11 dxgi d2d1 dwrite windowscodecs winhttp ws2_32 shell32 ole32 oleaut32 uuid gdi32 winmm sapi)
endif()
