cmake_minimum_required(VERSION 3.21)
project(PSVMS LANGUAGES CXX)

# C++17 is intentionally retained for broad compatibility with Qt 6.8 and
# third-party camera/FFmpeg SDKs. It is modern C++ and avoids an unnecessary
# language-standard migration while the application is being stabilized.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

option(PSVMS_ENABLE_WARNINGS "Enable useful compiler diagnostics" ON)

if(PSVMS_ENABLE_WARNINGS)
    if(MSVC)
        add_compile_options(/utf-8 /bigobj /W4 /permissive- /Zc:__cplusplus)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif()
elseif(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets Sql Network)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
endif()

if(FFMPEG_FOUND)
    set(FFMPEG_TARGET PkgConfig::FFMPEG)
else()
    add_library(ffmpeg_deps INTERFACE)
    foreach(lib avformat avcodec avutil)
        find_path(FF_${lib}_INC lib${lib}/${lib}.h)
        find_library(FF_${lib}_LIB ${lib})
        if(NOT FF_${lib}_INC OR NOT FF_${lib}_LIB)
            message(FATAL_ERROR "FFmpeg (${lib}) not found. Install FFmpeg development packages or configure CMAKE_PREFIX_PATH / vcpkg toolchain.")
        endif()
        target_include_directories(ffmpeg_deps INTERFACE ${FF_${lib}_INC})
        target_link_libraries(ffmpeg_deps INTERFACE ${FF_${lib}_LIB})
    endforeach()
    set(FFMPEG_TARGET ffmpeg_deps)
endif()

find_package(OpenSSL REQUIRED)

set(PSVMS_MODULES
    modules/app_dialogs.inc
    modules/app_pages.inc
    modules/app_shell.inc
    modules/app_playback_settings.inc
    modules/video.inc
    modules/core_services.inc
    modules/application_entry.inc
)

qt_add_executable(PSVMS WIN32 psvms.cpp branding.qrc ${PSVMS_MODULES})

# The implementation is now organized into subsystem modules. They are
# included by psvms.cpp intentionally for this first safe refactor: several
# legacy anonymous-namespace helpers are shared across sections. Moving them
# into separate translation units will follow after explicit interfaces exist.

target_link_libraries(PSVMS PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Sql
    Qt6::Network
    OpenSSL::Crypto
    ${FFMPEG_TARGET}
)

if(WIN32)
    target_sources(PSVMS PRIVATE app_icon.rc)
    target_link_libraries(PSVMS PRIVATE dwmapi psapi)

    # Embed the per-monitor DPI manifest through the MSVC linker.
    # Do not add RT_MANIFEST to app_icon.rc: LINK.exe already generates
    # an application manifest and CVTRES fails on duplicate RT_MANIFEST.
    if(MSVC)
        target_link_options(PSVMS PRIVATE
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/psvms.manifest"
        )
    endif()
endif()

# All interface icons and branding artwork are embedded into PSVMS.exe
# through branding.qrc (CMAKE_AUTORCC is enabled above).

# Reduce optimizer memory use while legacy implementation sections share one translation unit.
if(MSVC)
    # Diagnostic build: keep symbols and frame pointers so Windows crash dumps
    # can be resolved against the exact executable.
    target_compile_options(PSVMS PRIVATE /Zi /Oy-)
    target_link_options(PSVMS PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
    # Keep the existing low-memory optimization for normal Release builds.
    target_compile_options(PSVMS PRIVATE $<$<CONFIG:Release>:/O1>)
endif()
