cmake_minimum_required(VERSION 3.24)
# Set before project() so every target, including the bundled libraries' consumers, targets the same macOS.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
project(Compareel VERSION 1.0.0 LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

set(COMPAREEL_DISPLAY_VERSION "${PROJECT_VERSION}-dev" CACHE STRING "Displayed application version")
if(NOT "${COMPAREEL_DISPLAY_VERSION}" MATCHES "^[A-Za-z0-9.-]+$")
    message(FATAL_ERROR "Invalid display version")
endif()
# Numeric file version: major.minor.patch plus a pre-release number (1.0.0-rc3 -> 1,0,0,3).
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _version_core "${COMPAREEL_DISPLAY_VERSION}")
if(_version_core)
    set(_file_version "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3}")
else()
    set(_file_version "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH}")
endif()
set(COMPAREEL_FILE_VERSION "${_file_version},0")
if("${COMPAREEL_DISPLAY_VERSION}" MATCHES "-[A-Za-z]*\\.?([0-9]+)$")
    set(COMPAREEL_FILE_VERSION "${_file_version},${CMAKE_MATCH_1}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(compareel_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
        # Portable C library calls are used on purpose; the _s variants are MSVC-only.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Third-party libraries: pinned local SDKs on Windows (tools/Build-FFmpegMinimal.ps1, tools/Setup-Heif.ps1 +
# tools/Build-Heif.cmd), system packages found through pkg-config elsewhere.
add_library(compareel_ffmpeg INTERFACE)
add_library(compareel_heif INTERFACE)
if(WIN32)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/.deps/ffmpeg-8.1-min")
    if(NOT EXISTS "${FFMPEG_ROOT}/include/libavformat/avformat.h")
        message(FATAL_ERROR "FFmpeg SDK missing. Run tools/Build-FFmpegMinimal.ps1 first.")
    endif()
    target_include_directories(compareel_ffmpeg SYSTEM INTERFACE "${FFMPEG_ROOT}/include")
    foreach(component avformat avcodec avutil avfilter swresample swscale)
        target_link_libraries(compareel_ffmpeg INTERFACE "${FFMPEG_ROOT}/lib/${component}.lib")
    endforeach()
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    foreach(dll IN LISTS FFMPEG_DLLS)
        configure_file("${dll}" "${CMAKE_BINARY_DIR}/" COPYONLY)
    endforeach()

    set(HEIF_ROOT "${CMAKE_SOURCE_DIR}/.deps/libheif-1.23.7")
    if(NOT EXISTS "${HEIF_ROOT}/bin/heif.dll")
        message(FATAL_ERROR "libheif SDK missing. Run tools/Setup-Heif.ps1, then tools/Build-Heif.cmd.")
    endif()
    target_include_directories(compareel_heif SYSTEM INTERFACE "${HEIF_ROOT}/include")
    target_link_libraries(compareel_heif INTERFACE "${HEIF_ROOT}/lib/heif.lib")
    configure_file("${HEIF_ROOT}/bin/heif.dll" "${CMAKE_BINARY_DIR}/heif.dll" COPYONLY)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libavfilter libswresample libswscale)
    pkg_check_modules(LIBHEIF REQUIRED IMPORTED_TARGET libheif)
    target_link_libraries(compareel_ffmpeg INTERFACE PkgConfig::FFMPEG)
    target_link_libraries(compareel_heif INTERFACE PkgConfig::LIBHEIF)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Sql Svg Network)
# Qt before 6.8.4 links the AGL framework, which recent macOS SDKs no longer contain; nothing uses it.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_gl_libraries WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER _gl_libraries EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_gl_libraries}")
endif()

# Core: scanning, matching, playback and file operations. Operating-system calls live only in src/platform.
if(WIN32)
    set(COMPAREEL_PLATFORM_SOURCES
        src/platform/platform_win.cpp
        src/platform/file_ops_win.cpp
        src/platform/wic_win.cpp
        src/platform/audio_out_win.cpp
        src/platform/mf_video_win.cpp)
else()
    set(COMPAREEL_PLATFORM_SOURCES
        src/platform/platform_posix.cpp
        src/platform/file_ops_posix.cpp
        src/platform/audio_out_miniaudio.cpp
        src/platform/media_none.cpp)
    if(APPLE)
        list(APPEND COMPAREEL_PLATFORM_SOURCES src/platform/imageio_mac.cpp)
    endif()
endif()
add_library(compareel_core STATIC
    ${COMPAREEL_PLATFORM_SOURCES}
    src/PerceptualHash.cpp
    src/VideoScanner.cpp
    src/VideoScannerDiscovery.cpp
    src/VideoScannerGrouping.cpp
    src/VideoScannerSort.cpp
    src/VideoScannerAlignment.cpp
    src/VideoPlayer.cpp
    src/FileOperations.cpp
    src/FFmpegReader.cpp
    src/AudioEvidence.cpp
    src/PartialClipSearch.cpp
)
target_include_directories(compareel_core PUBLIC src)
compareel_warnings(compareel_core)
target_link_libraries(compareel_core PUBLIC compareel_ffmpeg)
if(WIN32)
    target_compile_definitions(compareel_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(compareel_core PUBLIC mfplat mfreadwrite mfuuid ole32 shell32 shlwapi winmm user32 gdi32 bcrypt windowscodecs)
else()
    # The POSIX platform layer uses QtCore for the trash, natural sorting and opening the file manager.
    target_include_directories(compareel_core PRIVATE third_party/miniaudio)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED) # Windows uses the zlib bundled with Qt
    target_link_libraries(compareel_core PUBLIC Qt6::Core Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
    if(APPLE)
        target_link_libraries(compareel_core PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" "-framework IOKit"
            "-framework CoreGraphics" "-framework ImageIO")
    else()
        # Inside a Flatpak, files go to the user's Trash through the desktop portal over D-Bus.
        find_package(Qt6 REQUIRED COMPONENTS DBus)
        target_link_libraries(compareel_core PUBLIC Qt6::DBus)
    endif()
endif()

if(WIN32)
    # Local runs resolve the same Qt runtime as the packages.
    get_target_property(QT_CORE_DLL Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT QT_CORE_DLL)
        get_target_property(QT_CORE_DLL Qt6::Core IMPORTED_LOCATION)
    endif()
    if(QT_CORE_DLL)
        get_filename_component(QT_BIN_DIR "${QT_CORE_DLL}" DIRECTORY)
        get_filename_component(QT_ROOT_DIR "${QT_BIN_DIR}" DIRECTORY)
        foreach(qt_dll Qt6Core Qt6Gui Qt6Widgets Qt6Concurrent Qt6Sql Qt6Svg)
            if(EXISTS "${QT_BIN_DIR}/${qt_dll}.dll")
                configure_file("${QT_BIN_DIR}/${qt_dll}.dll" "${CMAKE_BINARY_DIR}/${qt_dll}.dll" COPYONLY)
            endif()
        endforeach()
        foreach(plugin qgif qicns qico qjpeg qsvg qtga qtiff qwbmp qwebp)
            if(EXISTS "${QT_ROOT_DIR}/plugins/imageformats/${plugin}.dll")
                configure_file("${QT_ROOT_DIR}/plugins/imageformats/${plugin}.dll" "${CMAKE_BINARY_DIR}/imageformats/${plugin}.dll" COPYONLY)
            endif()
        endforeach()
        foreach(plugin platforms/qwindows sqldrivers/qsqlite styles/qmodernwindowsstyle)
            if(EXISTS "${QT_ROOT_DIR}/plugins/${plugin}.dll")
                configure_file("${QT_ROOT_DIR}/plugins/${plugin}.dll" "${CMAKE_BINARY_DIR}/${plugin}.dll" COPYONLY)
            endif()
        endforeach()
    endif()
endif()

set(COMPAREEL_IMAGE_SOURCES
    src/qt/ImageReader.cpp
    src/qt/ImageReaderWic.cpp
    src/qt/ImageReaderQt.cpp
    src/qt/ImageReaderFFmpeg.cpp
    src/qt/ImageReaderSvg.cpp
    src/qt/ImageReaderFormats.cpp
    src/qt/ImageReaderAnimation.cpp
    src/qt/HeifReader.cpp
)

# Every interface language (and the translators' template) is built into the program, so a macOS bundle or a
# Linux install has them all without a folder next to the executable; such a folder still adds or overrides one.
file(GLOB COMPAREEL_LANGUAGE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/languages/*.json")
set(COMPAREEL_LANGUAGE_QRC "<RCC><qresource prefix=\"/languages\">")
foreach(language IN LISTS COMPAREEL_LANGUAGE_FILES)
    get_filename_component(language_name "${language}" NAME)
    string(APPEND COMPAREEL_LANGUAGE_QRC "<file alias=\"${language_name}\">${language}</file>")
endforeach()
string(APPEND COMPAREEL_LANGUAGE_QRC "</qresource></RCC>\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/languages.qrc" CONTENT "${COMPAREEL_LANGUAGE_QRC}")

add_executable(Compareel WIN32 MACOSX_BUNDLE
    src/qt/main.cpp
    src/qt/MainWindow.cpp
    src/qt/MainWindowActions.cpp
    src/qt/MainWindowLayout.cpp
    src/qt/ResultsDelegate.cpp
    src/qt/ResultsView.cpp
    src/qt/Cleanup.cpp
    src/qt/Scanning.cpp
    src/qt/Dialogs.cpp
    src/qt/ScanDialogs.cpp
    src/qt/Review.cpp
    src/qt/BatchTransfer.cpp
    src/qt/PlayerPanel.cpp
    src/qt/ScanCache.cpp
    src/qt/Session.cpp
    src/qt/MainWindowChecksUi.cpp
    src/qt/MainWindowChecksScan.cpp
    src/qt/MainWindowChecksCleanup.cpp
    src/qt/MainWindowChecksPlayback.cpp
    ${COMPAREEL_IMAGE_SOURCES}
    src/qt/Updates.cpp
    src/qt/icons.qrc
    "${CMAKE_CURRENT_BINARY_DIR}/languages.qrc"
)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(Compareel PRIVATE src/qt/MacMenu.mm)
endif()
if(WIN32)
    target_sources(Compareel PRIVATE src/resource.rc)
    set_source_files_properties(src/resource.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
    # The manifest comes from resource.rc only. The linker's default one would be a second RT_MANIFEST resource #1;
    # on an English system both are tagged 0x0409 and the link fails (a Turkish system tags them apart).
    target_link_options(Compareel PRIVATE /MANIFEST:NO)
endif()
set_target_properties(Compareel PROPERTIES AUTORCC ON)
if(APPLE)
    # CFBundleVersion is numeric: 1.0.0-rc3 -> 1.0.0.3.
    string(REPLACE "," "." COMPAREEL_BUNDLE_VERSION "${COMPAREEL_FILE_VERSION}")
    configure_file(packaging/macos/Info.plist.in Info.plist @ONLY)
    target_sources(Compareel PRIVATE src/qt/branding/app.icns)
    set_source_files_properties(src/qt/branding/app.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(Compareel PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()
target_compile_definitions(Compareel PRIVATE COMPAREEL_DISPLAY_VERSION="${COMPAREEL_DISPLAY_VERSION}" COMPAREEL_FILE_VERSION=${COMPAREEL_FILE_VERSION})
compareel_warnings(Compareel)
target_link_libraries(Compareel PRIVATE compareel_core compareel_heif Qt6::Widgets Qt6::Concurrent Qt6::Sql Qt6::Svg Qt6::Network)

include(GNUInstallDirs)
install(TARGETS Compareel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/io.github.turxlab.Compareel.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/io.github.turxlab.Compareel.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    # The license texts travel inside the Linux package, as licenses.txt does in the Windows and macOS ones.
    set(COMPAREEL_LICENSE_DIR ${CMAKE_INSTALL_DATADIR}/licenses/io.github.turxlab.Compareel)
    install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${COMPAREEL_LICENSE_DIR})
    install(FILES third_party/ffmpeg/COPYING.LGPLv3 DESTINATION ${COMPAREEL_LICENSE_DIR} RENAME FFmpeg-COPYING.LGPLv3)
    install(FILES third_party/libheif-1.23.7/COPYING DESTINATION ${COMPAREEL_LICENSE_DIR} RENAME libheif-COPYING)
    install(FILES third_party/lucide/LICENSE DESTINATION ${COMPAREEL_LICENSE_DIR} RENAME Lucide-LICENSE)
    # Each hicolor size folder takes an icon of exactly that size.
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES src/qt/branding/app-${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME io.github.turxlab.Compareel.png)
    endforeach()
endif()

# Self-check programs (built on demand; tools/Run-Checks.ps1 drives them on Windows).
function(compareel_check name)
    cmake_parse_arguments(CHECK "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} EXCLUDE_FROM_ALL ${CHECK_SOURCES})
    target_include_directories(${name} PRIVATE src/qt)
    compareel_warnings(${name})
    target_link_libraries(${name} PRIVATE compareel_core ${CHECK_LIBS})
endfunction()

compareel_check(CoreChecks SOURCES tests/CoreChecks.cpp)
compareel_check(FFmpegChecks SOURCES tests/FFmpegChecks.cpp)
if(NOT WIN32)
    # These two take Windows-style wide arguments; a small main() converts UTF-8 arguments on POSIX.
    foreach(wide_target CoreChecks FFmpegChecks)
        target_sources(${wide_target} PRIVATE tests/PosixMain.cpp)
        target_compile_definitions(${wide_target} PRIVATE wmain=CompareelCheckMain)
    endforeach()
endif()
compareel_check(AudioEvidenceChecks SOURCES tests/AudioEvidenceChecks.cpp LIBS Qt6::Core)
compareel_check(PartialSearchChecks SOURCES tests/PartialSearchChecks.cpp LIBS Qt6::Core)
compareel_check(SubtitleChecks SOURCES tests/SubtitleChecks.cpp LIBS Qt6::Core)
compareel_check(ScanManifestChecks SOURCES tests/ScanManifestChecks.cpp LIBS Qt6::Core)
compareel_check(CacheRenderChecks SOURCES tests/CacheRenderChecks.cpp src/qt/ScanCache.cpp LIBS Qt6::Sql Qt6::Gui)
compareel_check(CacheCrashChecks SOURCES tests/CacheCrashChecks.cpp src/qt/ScanCache.cpp LIBS Qt6::Sql Qt6::Gui)
compareel_check(OperationRecoveryChecks SOURCES tests/OperationRecoveryChecks.cpp src/qt/ScanCache.cpp LIBS Qt6::Sql Qt6::Gui)
compareel_check(ImageChecks SOURCES tests/ImageChecks.cpp ${COMPAREEL_IMAGE_SOURCES} src/qt/ScanCache.cpp LIBS compareel_heif Qt6::Gui Qt6::Sql Qt6::Svg)
compareel_check(HeifChecks SOURCES tests/HeifChecks.cpp ${COMPAREEL_IMAGE_SOURCES} LIBS compareel_heif Qt6::Gui Qt6::Svg)
compareel_check(ImageGuardChecks SOURCES tests/ImageGuardChecks.cpp ${COMPAREEL_IMAGE_SOURCES} LIBS compareel_heif Qt6::Gui Qt6::Svg)
if(WIN32)
    compareel_check(ShellChecks SOURCES tests/ShellChecks.cpp LIBS Qt6::Gui)
endif()
set_target_properties(CoreChecks PROPERTIES EXCLUDE_FROM_ALL OFF)
