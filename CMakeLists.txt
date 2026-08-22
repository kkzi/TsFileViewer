cmake_minimum_required(VERSION 3.21)
project(TsFileViewer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

# ---- Qt ----------------------------------------------------------------
set(QTDIR $ENV{QTDIR} CACHE PATH "QTDIR")
set(Qt5_DIR ${QTDIR})
list(APPEND CMAKE_PREFIX_PATH ${QTDIR})
find_package(Qt5 CONFIG COMPONENTS Core Gui Widgets REQUIRED)

# ---- 3rd/tsfile (submodule) ---------------------------------------------
# Embedded Apache TsFile C++ tree. Static link, no tools/tests/examples.
#
# The cpp/ subtree assumes it is the top-level project: its sub-CMakeLists
# reference ${CMAKE_SOURCE_DIR} (cmake/CopyToDir.cmake, third_party/...,
# src include paths). add_subdirectory() from here would break those, so
# configure it as an ExternalProject into its own build tree instead, then
# import the resulting static lib via an IMPORTED target.
set(TSFILE_CPP_DIR "${CMAKE_SOURCE_DIR}/3rd/tsfile/cpp" CACHE PATH "Path to the TsFile C++ source tree" FORCE)
set(TSFILE_INCLUDE_DIR "${TSFILE_CPP_DIR}/src" CACHE PATH "Path to TsFile public headers" FORCE)

foreach(required_path IN ITEMS
        "${TSFILE_INCLUDE_DIR}"
        "${TSFILE_CPP_DIR}/CMakeLists.txt"
        "${TSFILE_CPP_DIR}/third_party/simde-0.8.4-rc3")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required TsFile path does not exist: ${required_path} (run 'git submodule update --init 3rd/tsfile')")
    endif()
endforeach()

include(ExternalProject)
set(TSFILE_BUILD_DIR "${CMAKE_BINARY_DIR}/embedded-tsfile")
set(TSFILE_LIB_DIR "${TSFILE_BUILD_DIR}/lib")
find_package(Threads REQUIRED)
ExternalProject_Add(tsfile-ext
    SOURCE_DIR        ${TSFILE_CPP_DIR}
    BINARY_DIR        ${TSFILE_BUILD_DIR}
    INSTALL_COMMAND   ""
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DTSFILE_BUILD_SHARED=OFF
        -DBUILD_TEST=OFF
        -DBUILD_TOOLS=OFF
        -DENABLE_ANTLR4=OFF
        -DCMAKE_CXX_STANDARD=14
    BUILD_BYPRODUCTS
        "${TSFILE_LIB_DIR}/tsfile.lib"
        "${TSFILE_LIB_DIR}/snappy.lib"
        "${TSFILE_LIB_DIR}/LZ4.lib"
        "${TSFILE_LIB_DIR}/lzokay.lib"
        "${TSFILE_LIB_DIR}/zlibstatic.lib"
        "${TSFILE_LIB_DIR}/zlibstaticd.lib")

add_library(tsfile INTERFACE IMPORTED)
add_dependencies(tsfile tsfile-ext)
target_include_directories(tsfile INTERFACE "${TSFILE_CPP_DIR}/src")
# Match the ENABLE_* switches the external build defaults to (snappy/lz4/
# lzokay on): CompressorFactory is header-only inline and its macros are
# decided by the including translation unit, so consumer must agree.
target_compile_definitions(tsfile INTERFACE ENABLE_SNAPPY ENABLE_LZ4 ENABLE_LZOKAY ENABLE_THREADS)
target_link_libraries(tsfile INTERFACE Threads::Threads)

# ---- 3rd/qcustomplot (submodule) ----------------------------------------
# legerch/QCustomPlot-library mirror at tag 2.1.1.1: upstream qcustomplot
# 2.1.1 sources wrapped in a CMake SHARED library target. Use only the lib/
# subproject and keep its output inside our build tree.
set(QCUSTOMPLOT_DIR "${CMAKE_SOURCE_DIR}/3rd/qcustomplot" CACHE PATH "Path to the QCustomPlot library tree" FORCE)
if(NOT EXISTS "${QCUSTOMPLOT_DIR}/lib/CMakeLists.txt")
    message(FATAL_ERROR "QCustomPlot not found (run 'git submodule update --init 3rd/qcustomplot')")
endif()
set(PROJECT_ARCH_TARGET "amd64")
set(PROJECT_BUILD_OUTPUT "${CMAKE_BINARY_DIR}/qcustomplot-output")
add_subdirectory("${QCUSTOMPLOT_DIR}/lib" "${CMAKE_BINARY_DIR}/qcustomplot")

# ---- TsFileViewer -------------------------------------------------------
file(GLOB SOURCES "Src/*.cpp" "Src/*.h")

add_executable(${PROJECT_NAME} ${SOURCES})
# Build the embedded tsfile before linking against its static lib.
add_dependencies(${PROJECT_NAME} tsfile-ext)

# GUI app: no console window.
set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${TSFILE_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/Src
)

# Import the externally built static lib (post-configure path is known).
# The compression libs (snappy/LZ4/lzokay/zlibstatic) are static targets
# inside the external build; link their outputs directly too.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${TSFILE_LIB_DIR}/tsfile.lib
    ${TSFILE_LIB_DIR}/snappy.lib
    ${TSFILE_LIB_DIR}/LZ4.lib
    ${TSFILE_LIB_DIR}/lzokay.lib
    ${TSFILE_LIB_DIR}/$<IF:$<CONFIG:Debug>,zlibstaticd.lib,zlibstatic.lib>
    qcustomplot
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
)

if(WIN32)
    # CommandLineToArgvW for UTF-8 argv rebuild in main. /Zc:__cplusplus is
    # required by the tsfile headers (STATIC_ASSERT checks __cplusplus >= 201103L,
    # and MSVC reports 199711L without this flag).
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32)
    target_compile_options(${PROJECT_NAME} PRIVATE /Zc:__cplusplus)
endif()

# Single-config Ninja: runtime output mirrors the sibling repos' layout.
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Build/${CMAKE_BUILD_TYPE}/bin"
)
# qcustomplot.dll lands next to the exe (copy from its own output dir).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/qcustomplot/qcustomplot.dll"
            "${CMAKE_SOURCE_DIR}/Build/${CMAKE_BUILD_TYPE}/bin/qcustomplot.dll"
)
