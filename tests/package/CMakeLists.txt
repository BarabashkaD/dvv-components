# Consumer of dvv_components; built by run-package-test.cmake. With DVV_COMPONENTS_SOURCE_DIR it
# takes dvv_components in-tree through FetchContent (as the app takes it; then DVV_BUILD_CMAKE_DIR
# names dvv-build's cmake folder), otherwise the installed package.
cmake_minimum_required(VERSION 4.2)
project(dvv_components_package_consumer LANGUAGES CXX)

if(DVV_COMPONENTS_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(dvv_components SOURCE_DIR "${DVV_COMPONENTS_SOURCE_DIR}")
    FetchContent_MakeAvailable(dvv_components)
else()
    find_package(dvv_components 0.1 REQUIRED)
endif()
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dvv::components)
dvv_set_compiler_options(consumer)
# Every component library the package was built with (whole archives, so the registrations
# survive).
dvv_components_link(consumer)
