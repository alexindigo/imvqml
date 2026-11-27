# SPDX-License-Identifier: LGPL-3.0-only
cmake_minimum_required(VERSION 3.24)
project(imvqml VERSION 0.1.4 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "imvqml requires Linux descriptor passing and shared memory")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips>=8.14)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vips-decode.cpp")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/vips-decode.cpp" IMV_LOADER_VERSION)
find_library(IMV_VIPS_PATH NAMES vips HINTS ${VIPS_LIBRARY_DIRS} REQUIRED)
qt_standard_project_setup(REQUIRES 6.8)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(qml_dir "${CMAKE_INSTALL_LIBDIR}/qt6/qml")
set(stage "${PROJECT_BINARY_DIR}/out")
file(RELATIVE_PATH decoder_from_lib "/prefix/${CMAKE_INSTALL_LIBDIR}"
    "/prefix/${CMAKE_INSTALL_LIBEXECDIR}/imvqml-decoder")

add_library(imv-wire STATIC src/private/wire.cpp src/private/decode-wire.cpp)
target_include_directories(imv-wire PUBLIC src/private)
target_link_libraries(imv-wire PUBLIC Qt6::Core)
set_target_properties(imv-wire PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(IMV_ENABLE_VIDEO "Build the runtime libmpv adapter" ON)
set(IMV_MPV_RUNTIME_HINT "" CACHE FILEPATH "Explicit libmpv.so.2 path instead of the dynamic loader search")
set(imv_mpv_runtime "")
add_executable(imvqml-decoder src/decoder.cpp src/vips-decode.cpp src/video-decode.cpp)
target_compile_definitions(imvqml-decoder PRIVATE QT_NO_KEYWORDS)
target_link_libraries(imvqml-decoder PRIVATE imv-wire PkgConfig::VIPS Qt6::Gui ${CMAKE_DL_LIBS})
if(IMV_ENABLE_VIDEO)
    find_path(IMV_MPV_HEADERS mpv/render.h)
    if(NOT IMV_MPV_HEADERS)
        message(FATAL_ERROR "Video needs mpv 0.35+ development headers (or set IMV_ENABLE_VIDEO=OFF)")
    endif()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${IMV_MPV_HEADERS}")
    unset(IMV_MPV_SOFTWARE_ABI CACHE)
    check_cxx_source_compiles([=[
        #include <mpv/client.h>
        #include <mpv/render.h>
        static_assert((MPV_CLIENT_API_VERSION >> 16) == 2);
        int main() {
            const char *backend = MPV_RENDER_API_TYPE_SW;
            mpv_render_param_type params[] = {MPV_RENDER_PARAM_SW_SIZE, MPV_RENDER_PARAM_SW_FORMAT,
                MPV_RENDER_PARAM_SW_STRIDE, MPV_RENDER_PARAM_SW_POINTER};
            return backend[0] == 0 || params[0] == MPV_RENDER_PARAM_INVALID;
        }
    ]=] IMV_MPV_SOFTWARE_ABI)
    unset(CMAKE_REQUIRED_INCLUDES)
    if(NOT IMV_MPV_SOFTWARE_ABI)
        message(FATAL_ERROR "mpv headers must expose client ABI major 2 and software rendering")
    endif()
    target_include_directories(imvqml-decoder PRIVATE "${IMV_MPV_HEADERS}")
    target_compile_definitions(imvqml-decoder PRIVATE IMV_HAVE_MPV_HEADERS)
    if(IMV_MPV_RUNTIME_HINT)
        if(NOT IS_ABSOLUTE "${IMV_MPV_RUNTIME_HINT}")
            message(FATAL_ERROR "IMV_MPV_RUNTIME_HINT must be an absolute path")
        endif()
        set(imv_mpv_runtime "${IMV_MPV_RUNTIME_HINT}")
        target_compile_definitions(imvqml-decoder PRIVATE IMV_MPV_RUNTIME_HINT="${IMV_MPV_RUNTIME_HINT}")
    else()
        find_library(IMV_MPV_RUNTIME NAMES libmpv.so.2)
        if(IMV_MPV_RUNTIME)
            set(imv_mpv_runtime "${IMV_MPV_RUNTIME}")
        endif()
    endif()
endif()
set_target_properties(imvqml-decoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY
    "${stage}/${CMAKE_INSTALL_LIBEXECDIR}")

add_library(imvqml SHARED)
add_library(imvqml::imvqml ALIAS imvqml)
qt_add_qml_module(imvqml URI ImvQml VERSION 1.0
    NO_PLUGIN_OPTIONAL
    OUTPUT_DIRECTORY "${stage}/${qml_dir}/ImvQml"
    SOURCES src/include/imvqml/imvimagejob.h src/imvimagejob.cpp src/private/helper.cpp
        src/private/pool.cpp src/private/pool.h src/private/decode-client.cpp src/private/export.cpp
        src/include/imvqml/imvimage.h src/imvimage.cpp src/private/image-pipeline.cpp
        src/private/image-node.cpp src/private/image-provider.cpp src/private/playback.cpp
        src/include/imvqml/imvcache.h src/imvcache.cpp src/private/image-store.cpp src/private/store-codec.cpp)
target_include_directories(imvqml PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE src/include/imvqml tests/vendor)
target_link_libraries(imvqml PUBLIC Qt6::Core Qt6::QmlIntegration Qt6::Quick
    PRIVATE Qt6::Gui Qt6::Qml imv-wire PkgConfig::WEBP ${CMAKE_DL_LIBS})
target_compile_definitions(imvqml PRIVATE IMV_DECODER_FROM_LIB="${decoder_from_lib}"
    IMV_STORE_VERSION="${PROJECT_VERSION}/${VIPS_VERSION}/${IMV_LOADER_VERSION}"
    IMV_VIPS_PATH="${IMV_VIPS_PATH}" IMV_MPV_PATH="${imv_mpv_runtime}")
set_target_properties(imvqml PROPERTIES VERSION "${PROJECT_VERSION}" SOVERSION 0
    LIBRARY_OUTPUT_DIRECTORY "${stage}/${CMAKE_INSTALL_LIBDIR}")
set_target_properties(imvqmlplugin PROPERTIES INSTALL_RPATH "$ORIGIN/../../..")

install(TARGETS imvqml EXPORT imvqmlTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS imvqmlplugin LIBRARY DESTINATION "${qml_dir}/ImvQml")
install(TARGETS imvqml-decoder RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
install(FILES "${stage}/${qml_dir}/ImvQml/qmldir"
    "${stage}/${qml_dir}/ImvQml/imvqml.qmltypes" DESTINATION "${qml_dir}/ImvQml")
install(DIRECTORY src/include/imvqml DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES README.md API.md Architecture.md FeatureMapping.md UPSTREAM.md LICENSE
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(DIRECTORY LICENSES DESTINATION "${CMAKE_INSTALL_DOCDIR}")

set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/imvqml")
configure_package_config_file(cmake/imvqmlConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/imvqmlConfig.cmake" INSTALL_DESTINATION "${package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/imvqmlConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(EXPORT imvqmlTargets NAMESPACE imvqml:: DESTINATION "${package_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/imvqmlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/imvqmlConfigVersion.cmake" DESTINATION "${package_dir}")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt Quick image pipeline and isolated decoder for Linux")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
