# Component libraries (spec §9.1; M4 design §4.4). Installed with dvv_components and included by its
# package configuration; in-tree the dvv_components project includes it. Needs
# dvv_set_compiler_options() (dvv_base, DvvCompilerOptions.cmake) and dvv_link_components()
# (dvv_registry, DvvComponents.cmake).
#
# dvv_add_component(<id> KIND <Kind> SOURCES <file>... [LINK <target>...]
#                   [REGISTERS <component id>...])
#   Option DVV_COMPONENT_<ID> (default ON; <ID> is <id> in upper case). When ON: a STATIC library
#   dvv_component_<id> (alias dvv::component_<id>, export name component_<id>) from SOURCES, linking
#   dvv::media PUBLIC and the LINK targets PRIVATE, with the dvv compiler options. REGISTERS (default
#   <id>) are the component ids its DVV_REGISTER lines register; each is recorded with KIND in the
#   global property DVV_COMPONENTS_REGISTERS as "<component id>=<Kind>", which the conformance test
#   compares with the registry (spec §12 "registry == enabled CMake set"), and the library is
#   appended to the global property DVV_COMPONENTS_LIBRARIES. When OFF: nothing is created or
#   recorded. <id>: [a-z][a-z0-9_]*; component ids: [a-z][a-z0-9_.-]*, each registered once; KIND: a
#   dvv::Kind name. No SHADERS in v1.0 (it comes with the first shader component, v1.x).
# dvv_components_link(<executable>)
#   dvv_link_components(<executable> <every library in DVV_COMPONENTS_LIBRARIES>): each as a whole
#   archive, so its DVV_REGISTER lines survive linking (with MSVC, dvv_link_components() also sets
#   /INCREMENTAL:NO).
# dvv_check_no_ffmpeg_imports(<test name> <target>...)
#   A CTest test that fails when one of the targets' binaries imports an FFmpeg library (M4 design
#   D3), through ${DVV_COMPONENTS_CMAKE_DIR}/check-no-ffmpeg-imports.cmake.
include_guard(GLOBAL)

function(dvv_add_component id)
    # Inside the function: include_guard(GLOBAL) skips a second include of this file (the installed
    # package found again from another directory), so a file-scope list would be empty there.
    set(DVV_COMPONENT_KINDS Source FormatDetector Exporter Tool Metric Analyser Processor)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "KIND" "SOURCES;LINK;REGISTERS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "dvv_add_component(${id}): unexpected '${arg_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT id MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "dvv_add_component(${id}): the id must match [a-z][a-z0-9_]*")
    endif()
    if(NOT arg_KIND IN_LIST DVV_COMPONENT_KINDS)
        list(JOIN DVV_COMPONENT_KINDS ", " kinds)
        message(FATAL_ERROR "dvv_add_component(${id}): KIND must be one of ${kinds}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "dvv_add_component(${id}): SOURCES is required")
    endif()
    if(NOT arg_REGISTERS)
        set(arg_REGISTERS "${id}")
    endif()
    string(TOUPPER "${id}" upper)
    option(DVV_COMPONENT_${upper} "Build the ${id} component library" ON)
    if(NOT DVV_COMPONENT_${upper})
        message(STATUS "dvv component ${id}: off (DVV_COMPONENT_${upper})")
        return()
    endif()
    get_property(registered GLOBAL PROPERTY DVV_COMPONENTS_REGISTERS)
    foreach(component IN LISTS arg_REGISTERS)
        if(NOT component MATCHES "^[a-z][a-z0-9_.-]*$")
            message(FATAL_ERROR "dvv_add_component(${id}): component id '${component}' must match "
                                "[a-z][a-z0-9_.-]*")
        endif()
        foreach(entry IN LISTS registered)
            string(REGEX REPLACE "=[A-Za-z]+$" "" registered_id "${entry}")
            if(registered_id STREQUAL component)
                message(FATAL_ERROR
                        "dvv_add_component(${id}): component '${component}' is registered twice")
            endif()
        endforeach()
    endforeach()

    set(target dvv_component_${id})
    add_library(${target} STATIC ${arg_SOURCES})
    add_library(dvv::component_${id} ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME component_${id})
    target_link_libraries(${target} PUBLIC dvv::media PRIVATE ${arg_LINK})
    dvv_set_compiler_options(${target})
    foreach(component IN LISTS arg_REGISTERS)
        set_property(GLOBAL APPEND PROPERTY DVV_COMPONENTS_REGISTERS "${component}=${arg_KIND}")
    endforeach()
    set_property(GLOBAL APPEND PROPERTY DVV_COMPONENTS_LIBRARIES ${target})
endfunction()

function(dvv_components_link target)
    get_property(libraries GLOBAL PROPERTY DVV_COMPONENTS_LIBRARIES)
    dvv_link_components(${target} ${libraries})
endfunction()

# dvv_check_no_ffmpeg_imports(<test name> <target>...)
#   Adds a CTest test that fails when the binary of one of the targets imports an FFmpeg library
#   (M4 design D3), reading the import tables with dumpbin (MSVC, next to the compiler) or readelf.
function(dvv_check_no_ffmpeg_imports test_name)
    if(MSVC)
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH compiler_dir)
        find_program(DVV_DUMPBIN dumpbin HINTS "${compiler_dir}" NO_DEFAULT_PATH REQUIRED)
        set(tool "-DDUMPBIN=${DVV_DUMPBIN}")
    else()
        find_program(DVV_READELF NAMES llvm-readelf-19 llvm-readelf readelf REQUIRED)
        set(tool "-DREADELF=${DVV_READELF}")
    endif()
    set(binaries "")
    foreach(target IN LISTS ARGN)
        list(APPEND binaries "$<TARGET_FILE:${target}>")
    endforeach()
    list(JOIN binaries "," binaries)
    add_test(NAME ${test_name}
             COMMAND ${CMAKE_COMMAND} "${tool}" "-DBINARIES=${binaries}"
                     -P "${DVV_COMPONENTS_CMAKE_DIR}/check-no-ffmpeg-imports.cmake")
endfunction()
