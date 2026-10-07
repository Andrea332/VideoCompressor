cmake_minimum_required(VERSION 3.21)

project(VideoCompressor VERSION 2.2.0 DESCRIPTION "Compress videos with a preview of the final size" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OPTIONAL_COMPONENTS Svg)
qt_standard_project_setup()

# Everything except main(), so that the tests can use it too
add_library(vc_core STATIC
    src/codecs.cpp src/codecs.h
    src/encodercheck.cpp src/encodercheck.h
    src/mainwindow.cpp src/mainwindow.h
    src/media.cpp src/media.h
    src/sizeestimator.cpp src/sizeestimator.h
    src/systeminfo.cpp src/systeminfo.h
)
target_include_directories(vc_core PUBLIC src)
target_link_libraries(vc_core PUBLIC Qt6::Widgets)
if(MSVC)
    target_compile_options(vc_core PUBLIC /utf-8 /W4 /permissive-)
endif()

qt_add_executable(video_compressor WIN32 src/main.cpp)
set_target_properties(video_compressor PROPERTIES OUTPUT_NAME VideoCompressor)
target_link_libraries(video_compressor PRIVATE vc_core)
target_compile_definitions(video_compressor PRIVATE APP_VERSION="${PROJECT_VERSION}")
if(WIN32)
    configure_file(resources/video_compressor.rc.in video_compressor.rc @ONLY)   # icon and version info
    target_sources(video_compressor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/video_compressor.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/video_compressor.rc" APPEND PROPERTY
                 OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icon.ico")
endif()

# Regenerates resources/icon.ico after editing the SVGs (not built by default):
#   cmake --build build --target make_icon
#   build\make_icon.exe resources\icon.svg resources\icon-small.svg resources\icon.ico
if(TARGET Qt6::Svg)
    qt_add_executable(make_icon tools/make_icon.cpp)
    set_target_properties(make_icon PROPERTIES EXCLUDE_FROM_ALL TRUE WIN32_EXECUTABLE FALSE)
    target_link_libraries(make_icon PRIVATE Qt6::Svg)
endif()

# ---------------------------------------------------------------- tests
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_video_compressor tests/tst_video_compressor.cpp)
    target_link_libraries(tst_video_compressor PRIVATE vc_core Qt6::Test)
    set_target_properties(tst_video_compressor PROPERTIES WIN32_EXECUTABLE FALSE)   # console output
    add_test(NAME tst_video_compressor COMMAND tst_video_compressor)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    set_tests_properties(tst_video_compressor PROPERTIES
        TIMEOUT 1800
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin};QT_QPA_PLATFORM=set:offscreen")
endif()

# ---------------------------------------------------------------- package
# FFmpeg to bundle: from the ffmpeg/ folder if present, otherwise the one in PATH (e.g. installed with winget)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/ffmpeg")
if(EXISTS "${FFMPEG_DIR}/ffmpeg.exe")
    set(_ffmpeg_bin "${FFMPEG_DIR}")
    set(_ffmpeg_docs "${FFMPEG_DIR}")
else()
    find_program(_ffmpeg_exe ffmpeg)
    if(_ffmpeg_exe)
        file(REAL_PATH "${_ffmpeg_exe}" _ffmpeg_real)   # follows the winget symlink
        get_filename_component(_ffmpeg_bin "${_ffmpeg_real}" DIRECTORY)
        get_filename_component(_ffmpeg_docs "${_ffmpeg_bin}" DIRECTORY)   # gyan.dev layout: <root>/bin/ffmpeg.exe
    endif()
endif()
if(_ffmpeg_bin AND EXISTS "${_ffmpeg_bin}/ffprobe.exe")
    message(STATUS "Bundling FFmpeg from ${_ffmpeg_bin}")
    # a "shared" build keeps its libraries in DLLs next to the exes (one copy for both)
    file(GLOB _ffmpeg_dlls "${_ffmpeg_bin}/*.dll")
    set(_ffmpeg_files "${_ffmpeg_bin}/ffmpeg.exe" "${_ffmpeg_bin}/ffprobe.exe" ${_ffmpeg_dlls})
    # the FFmpeg license (GPLv3) and build info must ship with the binaries
    foreach(_doc LICENSE README.txt)
        if(EXISTS "${_ffmpeg_docs}/${_doc}")
            list(APPEND _ffmpeg_files "${_ffmpeg_docs}/${_doc}")
        endif()
    endforeach()
    install(FILES ${_ffmpeg_files} DESTINATION ffmpeg)
    # same FFmpeg next to the exes in the build folder, so that tests and development runs use it too
    file(COPY ${_ffmpeg_files} DESTINATION "${CMAKE_BINARY_DIR}/ffmpeg")
else()
    message(WARNING "FFmpeg not found: put ffmpeg.exe and ffprobe.exe in ffmpeg/ or add them to PATH. "
                    "The package will not include FFmpeg.")
endif()

install(TARGETS video_compressor RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)
# Qt is used under the LGPLv3 (which builds on the GPLv3): its license must ship with the Qt DLLs
install(FILES licenses/Qt-LGPL-3.0-only.txt licenses/Qt-GPL-3.0-only.txt DESTINATION licenses)

# Qt libraries and plugins the app needs (no QML, no software OpenGL, no translations)
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin}" REQUIRED)
install(CODE "
    execute_process(COMMAND \"${WINDEPLOYQT}\" --release --no-translations --no-opengl-sw
                            --no-system-d3d-compiler --no-system-dxc-compiler --no-compiler-runtime
                            --no-network --skip-plugin-types tls,networkinformation,generic
                            \"\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:video_compressor>\"
                    COMMAND_ERROR_IS_FATAL ANY)
")

# Microsoft C++ runtime next to the exe, so that it runs without installing the redistributable
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)

# Packages: a portable zip and, if Inno Setup is installed, an installer
set(CPACK_VERBATIM_VARIABLES ON)   # values like {app}\VideoCompressor.exe are written as they are
set(CPACK_PACKAGE_NAME "Video Compressor")
set(CPACK_PACKAGE_VENDOR "Andrea Galet")
set(CPACK_PACKAGE_FILE_NAME "VideoCompressor-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Video Compressor")
set(CPACK_PACKAGE_EXECUTABLES VideoCompressor "Video Compressor")   # Start menu shortcut
set(CPACK_CREATE_DESKTOP_LINKS VideoCompressor)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR ZIP)

set(_pf86 "ProgramFiles(x86)")
find_program(INNO_SETUP_COMPILER ISCC HINTS "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6" "$ENV{${_pf86}}/Inno Setup 6"
                                            "$ENV{ProgramFiles}/Inno Setup 6")
if(INNO_SETUP_COMPILER)
    list(APPEND CPACK_GENERATOR INNOSETUP)
    set(CPACK_INNOSETUP_EXECUTABLE "${INNO_SETUP_COMPILER}")
    set(CPACK_INNOSETUP_ARCHITECTURE x64)
    set(CPACK_INNOSETUP_USE_MODERN_WIZARD ON)
    set(CPACK_INNOSETUP_ICON_FILE "${CMAKE_SOURCE_DIR}/resources/icon.ico")
    set(CPACK_INNOSETUP_LANGUAGES english italian)
    set(CPACK_INNOSETUP_IGNORE_LICENSE_PAGE ON)            # MIT: no need to accept anything
    set(CPACK_INNOSETUP_PROGRAM_MENU_FOLDER .)             # shortcut directly in the Start menu
    set(CPACK_INNOSETUP_RUN_EXECUTABLES VideoCompressor)   # "Launch Video Compressor" at the end
    # never change the AppId: it is how a newer version recognizes and updates the installed one
    set(CPACK_INNOSETUP_SETUP_AppId "{{74688896-6251-4C9E-BFE5-A7BEB66AF920}")
    set(CPACK_INNOSETUP_SETUP_AppVerName "Video Compressor ${PROJECT_VERSION}")
    set(CPACK_INNOSETUP_SETUP_UninstallDisplayIcon "{app}\\VideoCompressor.exe")
    set(CPACK_INNOSETUP_SETUP_MinVersion "10.0")
    # installs for the current user without admin rights; "all users" can be chosen in the first page
    set(CPACK_INNOSETUP_SETUP_PrivilegesRequired lowest)
    set(CPACK_INNOSETUP_SETUP_PrivilegesRequiredOverridesAllowed dialog)
    set(CPACK_INNOSETUP_SETUP_Compression lzma2/ultra64)
    set(CPACK_INNOSETUP_SETUP_SolidCompression ON)
else()
    message(STATUS "Inno Setup not found: only the zip package will be created")
endif()
include(CPack)
