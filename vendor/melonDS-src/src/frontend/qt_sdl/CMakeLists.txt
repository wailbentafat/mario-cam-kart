include(CMakeDependentOption)

include(FixInterfaceIncludes)

set(SOURCES_QT_SDL
    main.cpp
    main_shaders.h
    Screen.cpp
    Window.cpp
    EmuInstance.cpp
    EmuInstanceAudio.cpp
    EmuInstanceInput.cpp
    EmuThread.cpp
    CheatImportDialog.cpp
    CheatsDialog.cpp
    Config.cpp
    DateTimeDialog.cpp
    EmuSettingsDialog.cpp
    PowerManagement/PowerManagementDialog.cpp
    PowerManagement/resources/battery.qrc
    InputConfig/InputConfigDialog.cpp
    InputConfig/MapButton.h
    InputConfig/resources/ds.qrc
    VideoSettingsDialog.cpp
    CameraSettingsDialog.cpp
    AudioSettingsDialog.cpp
    FirmwareSettingsDialog.cpp
    PathSettingsDialog.cpp
    MPSettingsDialog.cpp
    WifiSettingsDialog.cpp
    InterfaceSettingsDialog.cpp
    ROMInfoDialog.cpp
    RAMInfoDialog.cpp
    TitleManagerDialog.cpp
    OSD_shaders.h
    font.h
    Platform.cpp
    Platform_AAC.cpp
    QPathInput.h
    SaveManager.cpp
    CameraManager.cpp
    AboutDialog.cpp
    AboutDialog.h
    AboutDialog.ui
    ARCodeQDecl.h

    ArchiveUtil.h
    ArchiveUtil.cpp

    ../ScreenLayout.cpp
    ../mic_blow.h

    ../glad/glad.c
    ../duckstation/gl/context.cpp

    ${CMAKE_SOURCE_DIR}/res/melon.qrc

    CLI.h
    CLI.cpp

    LANDialog.cpp
    NetplayDialog.cpp

    GestureController.h
    GestureController.cpp
    PhoneController.h
    PhoneController.cpp
    Ds64Python.cpp
)

option(USE_QT6 "Use Qt 6 instead of Qt 5" ON)

if (USE_QT6)
    find_package(Qt6 COMPONENTS Core Gui Widgets Network Multimedia OpenGL OpenGLWidgets Svg REQUIRED)
    set(QT_LINK_LIBS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::OpenGL Qt6::OpenGLWidgets)
else()
    find_package(Qt5 COMPONENTS Core Gui Widgets Network Multimedia Svg REQUIRED)
    set(QT_LINK_LIBS Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::Multimedia)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

if (BUILD_STATIC)
    list(APPEND PKG_CONFIG_EXECUTABLE "--static")
endif()

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LibArchive REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(Zstd REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(Faad REQUIRED IMPORTED_TARGET faad2)

# Embedded Python for GestureController (webcam hand-tracking via MediaPipe).
# Dev-build only for now: points straight at tools/gesture in the source
# tree rather than bundling it into an app bundle/installer.
find_package(Python3 REQUIRED COMPONENTS Development)
add_compile_definitions(GESTURE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/gesture")
add_compile_definitions(PHONE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/phone")

# play.py creates .venv with `python -m venv`, whose site-packages layout
# differs by OS: Windows uses .venv/Lib/site-packages (no version subdir,
# capital L); POSIX uses .venv/lib/pythonX.Y/site-packages. Derive the
# version subdir from the same Python3 find_package() just resolved
# above, so this stays correct as long as the venv is created with that
# same interpreter (which play.py ensures).
if(WIN32)
    set(DS64_VENV_SITE_PACKAGES "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.venv/Lib/site-packages")
else()
    set(DS64_VENV_SITE_PACKAGES "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.venv/lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages")
endif()
add_compile_definitions(GESTURE_VENV_SITE_PACKAGES="${DS64_VENV_SITE_PACKAGES}")

# DS64 releases are standalone: the ROM is compiled into the executable (as a
# Qt resource) so a downloaded .exe/.app/AppImage doesn't need a sidecar .nds.
# Off by default for normal source builds; CI turns it on. If no explicit file
# is given, the .nds at the root of this repo (two levels above the melonDS
# source dir) is used.
option(DS64_EMBED_ROM "Embed a Nintendo DS ROM into the melonDS executable" OFF)
set(DS64_EMBED_ROM_FILE "" CACHE FILEPATH "Explicit .nds file to embed (defaults to the repo-root ROM)")

if (DS64_EMBED_ROM)
    if (NOT DS64_EMBED_ROM_FILE)
        file(GLOB DS64_REPO_ROMS "${CMAKE_SOURCE_DIR}/../../*.nds")
        list(SORT DS64_REPO_ROMS)
        list(LENGTH DS64_REPO_ROMS DS64_REPO_ROM_COUNT)
        if (DS64_REPO_ROM_COUNT EQUAL 0)
            message(FATAL_ERROR "DS64_EMBED_ROM is ON, but no .nds file was found in the repo root. "
                                "Set DS64_EMBED_ROM_FILE explicitly.")
        endif()
        list(GET DS64_REPO_ROMS 0 DS64_EMBED_ROM_FILE)
    endif()

    get_filename_component(DS64_EMBED_ROM_FILE "${DS64_EMBED_ROM_FILE}" ABSOLUTE)
    if (NOT EXISTS "${DS64_EMBED_ROM_FILE}")
        message(FATAL_ERROR "DS64_EMBED_ROM_FILE does not exist: ${DS64_EMBED_ROM_FILE}")
    endif()
    if (IS_DIRECTORY "${DS64_EMBED_ROM_FILE}")
        message(FATAL_ERROR "DS64_EMBED_ROM_FILE is a directory: ${DS64_EMBED_ROM_FILE}")
    endif()

    file(SHA256 "${DS64_EMBED_ROM_FILE}" DS64_EMBED_ROM_HASH)
    # rcc needs forward slashes in the .qrc on every platform (including
    # Windows), and the path has to be XML-escaped.
    file(TO_CMAKE_PATH "${DS64_EMBED_ROM_FILE}" DS64_EMBED_ROM_QRC_PATH)
    string(REPLACE "&" "&amp;" DS64_EMBED_ROM_QRC_PATH "${DS64_EMBED_ROM_QRC_PATH}")
    string(REPLACE "<" "&lt;" DS64_EMBED_ROM_QRC_PATH "${DS64_EMBED_ROM_QRC_PATH}")
    string(REPLACE ">" "&gt;" DS64_EMBED_ROM_QRC_PATH "${DS64_EMBED_ROM_QRC_PATH}")
    set(DS64_EMBED_ROM_QRC "${CMAKE_CURRENT_BINARY_DIR}/ds64_rom.qrc")
    file(WRITE "${DS64_EMBED_ROM_QRC}"
        "<!DOCTYPE RCC>\n"
        "<RCC version=\"1.0\">\n"
        "    <qresource prefix=\"/ds64\">\n"
        "        <file alias=\"game.nds\">${DS64_EMBED_ROM_QRC_PATH}</file>\n"
        "    </qresource>\n"
        "</RCC>\n"
    )
    message(STATUS "DS64: embedding ROM ${DS64_EMBED_ROM_FILE} (sha256 ${DS64_EMBED_ROM_HASH})")
endif()

fix_interface_includes(PkgConfig::SDL2 PkgConfig::LibArchive)

add_compile_definitions(ARCHIVE_SUPPORT_ENABLED)

add_executable(melonDS ${SOURCES_QT_SDL})

if (DS64_EMBED_ROM)
    target_sources(melonDS PRIVATE "${DS64_EMBED_ROM_QRC}")
    target_compile_definitions(melonDS PRIVATE
        DS64_EMBEDDED_ROM_PATH=":/ds64/game.nds"
        DS64_EMBEDDED_ROM_HASH="${DS64_EMBED_ROM_HASH}"
    )
endif()

add_subdirectory("../../net"
    ${CMAKE_BINARY_DIR}/net)

target_link_libraries(melonDS PRIVATE net-utils)

if (WIN32)
    target_link_libraries(melonDS PUBLIC opengl32)

    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_wgl.cpp

        ../glad/glad_wgl.c
    )

    if (MINGW AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET melonDS PROPERTY AUTORCC_OPTIONS "--no-zstd")
    endif()

    if (BUILD_STATIC)
        find_package(SDL2 CONFIG COMPONENTS SDL2main)
        if (TARGET SDL2::SDL2main)
            target_link_libraries(melonDS PRIVATE SDL2::SDL2main)
        endif()
    endif()
elseif (APPLE)
    if (NOT USE_QT6)
        find_library(COCOA_LIB Cocoa)
        target_link_libraries(melonDS PRIVATE ${COCOA_LIB})
    endif()
    find_library(OPENGL_LIB OpenGL)
    target_link_libraries(melonDS PRIVATE ${OPENGL_LIB})
    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_agl.mm
    )
    set_source_files_properties(
        ../duckstation/gl/context_agl.mm
        PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations"
    )
else()
    find_package(X11 REQUIRED)
    find_package(EGL REQUIRED)

    option(ENABLE_WAYLAND "Enable Wayland support" ON)

    if (ENABLE_WAYLAND)
        # we only need ECM for Wayland
        # so we only require it from here
        find_package(ECM REQUIRED NO_MODULE)
        list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")

        find_package(Wayland REQUIRED Client)

        target_compile_definitions(melonDS PRIVATE WAYLAND_ENABLED)

        target_sources(melonDS PRIVATE
            ../duckstation/gl/context_egl_wayland.cpp
        )
    endif()

    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_egl.cpp
        ../duckstation/gl/context_egl_x11.cpp
        ../duckstation/gl/context_glx.cpp
        ../duckstation/gl/x11_window.cpp

        ../glad/glad_egl.c
        ../glad/glad_glx.c
    )
    target_link_libraries(melonDS PRIVATE "${X11_LIBRARIES}" "${EGL_LIBRARIES}")
    target_include_directories(melonDS PRIVATE "${X11_INCLUDE_DIR}")
    add_compile_definitions(QAPPLICATION_CLASS=QApplication)
endif()


if (BUILD_STATIC)
    qt_import_plugins(melonDS INCLUDE Qt::QSvgPlugin)
    if (WIN32 AND USE_QT6)
        qt_import_plugins(melonDS INCLUDE Qt::QModernWindowsStylePlugin)
    endif()
    if (USE_VCPKG AND UNIX AND NOT APPLE)
        pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
        target_link_libraries(melonDS PRIVATE PkgConfig::ALSA)
    elseif (NOT APPLE)
        target_link_options(melonDS PRIVATE -static)
    endif()
endif()

target_include_directories(melonDS PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")

if (USE_QT6)
    target_include_directories(melonDS PUBLIC ${Qt6Gui_PRIVATE_INCLUDE_DIRS})
else()
    target_include_directories(melonDS PUBLIC ${Qt5Gui_PRIVATE_INCLUDE_DIRS})
endif()
target_link_libraries(melonDS PRIVATE core)
target_link_libraries(melonDS PRIVATE PkgConfig::SDL2 PkgConfig::LibArchive PkgConfig::Zstd PkgConfig::Faad)
target_link_libraries(melonDS PRIVATE ${QT_LINK_LIBS} ${CMAKE_DL_LIBS})
target_link_libraries(melonDS PRIVATE Python3::Python)

if (WIN32)
    option(PORTABLE "Make a portable build that looks for its configuration in the current directory" ON)

    if (PORTABLE)
        target_compile_definitions(melonDS PRIVATE WIN32_PORTABLE)
    endif()

    string(REPLACE . , MELON_RC_VERSION ${melonDS_VERSION})
    configure_file("${CMAKE_SOURCE_DIR}/res/melon.rc.in" "${CMAKE_BINARY_DIR}/res/melon.rc")
    target_sources(melonDS PUBLIC "${CMAKE_BINARY_DIR}/res/melon.rc")
    target_include_directories(melonDS PRIVATE "${CMAKE_BINARY_DIR}/res")

    if (${ARCHITECTURE} STREQUAL x86_64)
        set(WIN32_ARCHITECTURE amd64)
    elseif (${ARCHITECTURE} STREQUAL x86)
        set(WIN32_ARCHITECTURE x86)
    elseif (${ARCHITECTURE} MATCHES "ARM(64)?")
        string(TOLOWER "${ARCHITECTURE}" WIN32_ARCHITECTURE)
    endif()


    target_link_libraries(melonDS PRIVATE ws2_32 iphlpapi)
    set_target_properties(melonDS PROPERTIES WIN32_EXECUTABLE $<IF:$<CONFIG:Debug>,OFF,ON>)

    if (MINGW)
        configure_file("${CMAKE_SOURCE_DIR}/res/xp.manifest.in" "${CMAKE_BINARY_DIR}/res/xp.manifest")
    else()
        # Work around faad2 unconditionally linking libm even on Windows
        if (USE_VCPKG)
            add_library(m STATIC)
            target_link_libraries(melonDS PUBLIC m)
        endif()
    endif()
endif()

if (APPLE)
    # Copy icon into the bundle
    set(RESOURCE_FILES "${CMAKE_SOURCE_DIR}/res/melon.icns")
    target_sources(melonDS PUBLIC "${RESOURCE_FILES}")

    set_target_properties(melonDS PROPERTIES
        MACOSX_BUNDLE true
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/res/melon.plist.in
        OUTPUT_NAME melonDS
        RESOURCE "${RESOURCE_FILES}")


    option(MACOS_BUNDLE_LIBS "Bundle libraries with the app on macOS" OFF)
    option(MACOS_BUILD_DMG "Build DMG image of the macOS application bundle" OFF)

    if (MACOS_BUNDLE_LIBS)
        if (MACOS_BUILD_DMG)
            set(DMGARG "--dmg")
        endif()
        add_custom_command(TARGET melonDS POST_BUILD
            COMMAND ${CMAKE_SOURCE_DIR}/tools/mac-libs.rb ${DMGARG} ${CMAKE_BINARY_DIR}
            COMMENT "Bundling macOS libraries..."
            VERBATIM)
    endif()
endif()

if (UNIX AND NOT APPLE)
    foreach(SIZE 16 32 48 64 128 256)
        install(FILES ${CMAKE_SOURCE_DIR}/res/icon/melon_${SIZE}x${SIZE}.png
            DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/${SIZE}x${SIZE}/apps
            RENAME net.kuribo64.melonDS.png)
    endforeach()

    install(FILES ${CMAKE_SOURCE_DIR}/res/net.kuribo64.melonDS.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)
    install(TARGETS melonDS BUNDLE DESTINATION ${CMAKE_BINARY_DIR} RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

    if (NOT USE_QT6)
        set_target_properties(melonDS PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    endif()
elseif(APPLE)
    install(TARGETS melonDS BUNDLE DESTINATION "${CMAKE_INSTALL_PREFIX}/Applications")
endif()

if (ENABLE_OGLRENDERER)
    set(MELONDS_GL_HEADER \"frontend/glad/glad.h\" CACHE STRING "Path to a header that contains OpenGL function and type declarations.")

    target_compile_definitions(melonDS PUBLIC OGLRENDERER_ENABLED)
    target_compile_definitions(melonDS PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})
    target_compile_definitions(core PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})
endif()
