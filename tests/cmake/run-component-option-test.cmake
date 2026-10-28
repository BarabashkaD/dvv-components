# Configures tests/cmake/option-project once per case and checks what dvv_add_component() and
# dvv_components_link() did (M4 design §4.4). Called by dvv_components.cmake_functions with
# -DPROJECT_DIR, -DWORK_DIR, -DGENERATOR, -DPLATFORM (may be empty), -DCXX_COMPILER,
# -DCOMPONENT_MODULE and -DREGISTRY_MODULE.
cmake_minimum_required(VERSION 4.2)
foreach(variable IN ITEMS PROJECT_DIR WORK_DIR GENERATOR CXX_COMPILER COMPONENT_MODULE
                          REGISTRY_MODULE)
    if("${${variable}}" STREQUAL "")
        message(FATAL_ERROR "pass -D${variable}")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")

# check(<case> <0|1 expected failure> <expected output regex> [-D<name>=<value>...])
function(check case expect_failure expected_text)
    set(command "${CMAKE_COMMAND}" -S "${PROJECT_DIR}" -B "${WORK_DIR}/${case}" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCOMPONENT_MODULE=${COMPONENT_MODULE}"
        "-DREGISTRY_MODULE=${REGISTRY_MODULE}" "-DCASE=${case}" ${ARGN})
    if(PLATFORM)
        list(APPEND command -A "${PLATFORM}")
    endif()
    execute_process(COMMAND ${command} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    # CMake wraps long messages; compare with every run of white space folded into one space.
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${out}${err}")
    if(expect_failure AND status EQUAL 0)
        message(FATAL_ERROR "FAIL: case ${case} configured")
    elseif(NOT expect_failure AND NOT status EQUAL 0)
        message(FATAL_ERROR "FAIL: case ${case} failed:\n${out}${err}")
    elseif(NOT output MATCHES "${expected_text}")
        message(FATAL_ERROR "FAIL: case ${case} lacks '${expected_text}':\n${out}${err}")
    endif()
endfunction()

set(link_a "\\$<LINK_LIBRARY:WHOLE_ARCHIVE,dvv_component_probe_a>")
set(link_b "\\$<LINK_LIBRARY:WHOLE_ARCHIVE,dvv_component_probe_b>")
check(default 0 "registers=<probe\\.a=Source;probe\\.b=Source;probe_b=Tool> libraries=<dvv_component_probe_a;dvv_component_probe_b> probe_a_target=<yes> app_links=<${link_a};${link_b}>")
check(probe_a_off 0 "dvv component probe_a: off \\(DVV_COMPONENT_PROBE_A\\).*registers=<probe_b=Tool> libraries=<dvv_component_probe_b> probe_a_target=<no> app_links=<${link_b}>"
      -DDVV_COMPONENT_PROBE_A=OFF)
check(bad_kind 1 "dvv_add_component\\(probe_bad\\): KIND must be one of Source, FormatDetector, Exporter, Tool, Metric, Analyser, Processor")
check(bad_id 1 "dvv_add_component\\(Probe-Bad\\): the id must match")
check(twice 1 "dvv_add_component\\(probe_b\\): component 'probe\\.same' is registered twice")
message(STATUS "component option tests passed")
