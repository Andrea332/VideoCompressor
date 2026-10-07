cmake_minimum_required(VERSION 3.21)

project(VideoCompressor VERSION 2.0.0 DESCRIPTION "Compress videos with a preview of the final size" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
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
target_link_libraries(video_compressor PRIVATE vc_core)
target_compile_definitions(video_compressor PRIVATE APP_VERSION="${PROJECT_VERSION}")

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
    install(PROGRAMS "${_ffmpeg_bin}/ffmpeg.exe" "${_ffmpeg_bin}/ffprobe.exe" DESTINATION ffmpeg)
    # the FFmpeg license (GPLv3) and build info must ship with the binaries
    foreach(_doc LICENSE README.txt)
        if(EXISTS "${_ffmpeg_docs}/${_doc}")
            install(FILES "${_ffmpeg_docs}/${_doc}" DESTINATION ffmpeg)
        endif()
    endforeach()
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
                            \"\${CMAKE_INSTALL_PREFIX}/video_compressor.exe\"
                    COMMAND_ERROR_IS_FATAL ANY)
")

# Microsoft C++ runtime next to the exe, so that it runs without installing the redistributable
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "VideoCompressor-${PROJECT_VERSION}-win64")
include(CPack)
