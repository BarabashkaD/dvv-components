cmake_minimum_required(VERSION 4.2)
project(dvv_components VERSION 0.1.1 DESCRIPTION "dvv component libraries" LANGUAGES CXX)
# Version string; a pre-release build adds its release-stage label.
set(dvv_components_VERSION_STRING "${PROJECT_VERSION}")

# dvv-build provides the shared CMake modules and the dependency check (dvv-deps.txt); the dvv
# presets set DVV_BUILD_CMAKE_DIR to its cmake folder.
if(NOT DVV_BUILD_CMAKE_DIR OR NOT IS_ABSOLUTE "${DVV_BUILD_CMAKE_DIR}"
   OR NOT EXISTS "${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
    message(FATAL_ERROR "dvv_components: DVV_BUILD_CMAKE_DIR is not set, not absolute or "
                        "has no DvvDeps.cmake (it must be dvv-build's cmake folder); configure "
                        "with a dvv-build preset (e.g. `cmake --preset vs2026`, see README.md) "
                        "or pass -DDVV_BUILD_CMAKE_DIR=<absolute path of dvv-build>/cmake")
endif()
include("${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
# Adds dvv-base, dvv-registry, dvv-platform, dvv-gpu and dvv-media (dvv-deps.txt) and checks their
# versions; a stand-alone build tests only this repository.
dvv_dependencies()

option(DVV_BUILD_TESTS "Build the dvv tests" ${PROJECT_IS_TOP_LEVEL})

# dvv-base sets DVV_CMAKE_DIR to the shared modules (in-tree: dvv-build's cmake folder).
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Higher repositories use "${DVV_COMPONENTS_CMAKE_DIR}/DvvComponent.cmake" (in-tree here, installed
# through the package configuration).
set(DVV_COMPONENTS_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake" CACHE INTERNAL
    "dvv-components CMake modules")
include("${DVV_COMPONENTS_CMAKE_DIR}/DvvComponent.cmake")

# What an application links besides dvv_components_link(<exe>).
add_library(dvv_components INTERFACE)
add_library(dvv::components ALIAS dvv_components)
set_target_properties(dvv_components PROPERTIES EXPORT_NAME components)
target_link_libraries(dvv_components INTERFACE dvv::media)

# FFmpeg of the third-party prefix: headers at build time, libraries at run time (M4 design D3).
find_package(dvv_ffmpeg_sdk REQUIRED CONFIG)

# dvv_ffmpeg's private headers and FFmpeg's headers (system headers), for dvv_ffmpeg, the component
# libraries and the tests only. Never installed or exported, so no FFmpeg header reaches an
# application: link it as $<BUILD_INTERFACE:dvv_ffmpeg_internal>.
add_library(dvv_ffmpeg_internal INTERFACE)
target_include_directories(dvv_ffmpeg_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dvv_ffmpeg_internal INTERFACE dvv::ffmpeg_headers)

# The FFmpeg run-time binding (M4 design D3, §4.4): loader, function table, AVERROR mapping and log
# routing. Public part: dvv/components/ffmpeg.h (status and loading; no FFmpeg types).
add_library(dvv_ffmpeg STATIC
    src/ffmpeg/checks.cpp
    src/ffmpeg/error.cpp
    src/ffmpeg/loader.cpp)
add_library(dvv::ffmpeg ALIAS dvv_ffmpeg)
set_target_properties(dvv_ffmpeg PROPERTIES EXPORT_NAME ffmpeg)
target_include_directories(dvv_ffmpeg PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dvv_ffmpeg
    PUBLIC dvv::registry
    PRIVATE dvv::media dvv::platform $<BUILD_INTERFACE:dvv_ffmpeg_internal>)
dvv_set_compiler_options(dvv_ffmpeg)
target_link_libraries(dvv_components INTERFACE dvv::ffmpeg)

# Component libraries (dvv_add_component) go here, before the install rules and the tests: the
# installed package and the conformance test list them.

# FFmpeg source providers (design v2 §4.4): loaded at run time, never linked (D3).
dvv_add_component(ffmpeg KIND Source
    SOURCES src/sources/ffmpeg_rules.cpp src/sources/frame_ring.cpp src/sources/av_worker.cpp
            src/sources/container_supplier.cpp src/sources/ffmpeg_provider.cpp
            src/sources/graph_supplier.cpp src/sources/lavfi_provider.cpp
    LINK dvv::ffmpeg $<BUILD_INTERFACE:dvv_ffmpeg_internal>
    REGISTERS ffmpeg lavfi)

get_property(component_libraries GLOBAL PROPERTY DVV_COMPONENTS_LIBRARIES)
install(TARGETS dvv_components dvv_ffmpeg ${component_libraries} EXPORT dvv_componentsTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/dvv_components)
install(EXPORT dvv_componentsTargets NAMESPACE dvv:: DESTINATION ${config_dir})
set(DVV_COMPONENTS_INSTALLED_LIBRARIES "")
foreach(library IN LISTS component_libraries)
    string(REGEX REPLACE "^dvv_" "dvv::" exported "${library}")
    list(APPEND DVV_COMPONENTS_INSTALLED_LIBRARIES "${exported}")
endforeach()
configure_package_config_file(cmake/dvv_componentsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_componentsConfig.cmake INSTALL_DESTINATION ${config_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/dvv_componentsConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_componentsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_componentsConfigVersion.cmake
    cmake/DvvComponent.cmake
    cmake/check-no-ffmpeg-imports.cmake
    DESTINATION ${config_dir})

if(DVV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
