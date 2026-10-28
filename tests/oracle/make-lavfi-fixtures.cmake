# The lavfi reference fixtures of the lavfi provider tests (M4 Task 5b): each graph run through the
# prefix's ffmpeg into raw frames.
#
#   cmake -DFFMPEG=<ffmpeg program> -DOUT=<dir> [-DLIBDIR=<FFmpeg library dir>] -P <this file>
#
# Writes into <OUT>/<version>/ (<version>: the third token of `ffmpeg -version` line 1, every
# character outside [A-Za-z0-9._-] removed) and records that absolute folder in <OUT>/current.txt;
# a done.stamp there written by the same script skips the work. Without the program: prints "ffmpeg
# program missing" and exits 0 without a stamp. Outside Windows every command runs with
# LD_LIBRARY_PATH=<LIBDIR>.
#
# Output: manifest.txt, one line per graph "<k>|<graph>|<frames>", and lavfi_<k>.raw with the
# graph's first <frames> frames as `ffmpeg -f rawvideo` writes them (tight planes, frame after
# frame).
cmake_minimum_required(VERSION 4.2)

if(NOT OUT)
    message(FATAL_ERROR "make-lavfi-fixtures: OUT is required")
endif()
if(NOT FFMPEG OR NOT EXISTS "${FFMPEG}")
    message("ffmpeg program missing")
    return()
endif()

set(command_prefix "")
if(NOT CMAKE_HOST_WIN32 AND LIBDIR)
    set(command_prefix "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${LIBDIR}")
endif()

execute_process(COMMAND ${command_prefix} "${FFMPEG}" -hide_banner -version
                RESULT_VARIABLE result OUTPUT_VARIABLE version_text ERROR_VARIABLE version_text)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "make-lavfi-fixtures: ffmpeg -version failed (${result}):\n${version_text}")
endif()
string(REGEX MATCH "^[^\n]*" first_line "${version_text}")
string(REGEX REPLACE " +" ";" tokens "${first_line}")
list(LENGTH tokens token_count)
if(token_count LESS 3)
    message(FATAL_ERROR "make-lavfi-fixtures: unexpected ffmpeg -version line '${first_line}'")
endif()
list(GET tokens 2 version)
string(REGEX REPLACE "[^A-Za-z0-9._-]" "" version "${version}")
if(version STREQUAL "")
    message(FATAL_ERROR "make-lavfi-fixtures: no version in '${first_line}'")
endif()

set(dir "${OUT}/${version}")
file(MAKE_DIRECTORY "${OUT}")
file(WRITE "${OUT}/current.txt" "${dir}")
# The stamp holds the version and this script's hash, so a changed script makes the files again.
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" script_hash)
set(stamp "${version} ${script_hash}\n")
if(EXISTS "${dir}/done.stamp")
    file(READ "${dir}/done.stamp" existing_stamp)
    if(existing_stamp STREQUAL stamp)
        message(STATUS "make-lavfi-fixtures: ${dir} is complete")
        return()
    endif()
endif()
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")

# The reference graphs: planar 8/10/16-bit, semi-planar and packed layouts.
set(frames 24)
set(graphs
    "testsrc2=s=352x288:r=25,format=yuv420p"
    "testsrc2=s=352x288:r=25,format=yuv422p10le"
    "testsrc2=s=352x288:r=25,format=nv12"
    "testsrc2=s=352x288:r=25,format=rgb24"
    "testsrc2=s=352x288:r=25,format=yuyv422"
    "testsrc2=s=352x288:r=25,format=gray16le")

set(manifest "")
set(k 0)
foreach(graph IN LISTS graphs)
    math(EXPR k "${k} + 1")
    execute_process(COMMAND ${command_prefix} "${FFMPEG}" -hide_banner -nostdin -y -f lavfi
                            -i "${graph}" -frames:v ${frames} -fps_mode passthrough -f rawvideo
                            lavfi_${k}.raw
                    WORKING_DIRECTORY "${dir}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "make-lavfi-fixtures: ffmpeg -f lavfi -i ${graph} failed (${result}):\n"
                            "${output}")
    endif()
    string(APPEND manifest "${k}|${graph}|${frames}\n")
endforeach()
file(WRITE "${dir}/manifest.txt" "${manifest}")

file(WRITE "${dir}/done.stamp" "${stamp}")
message(STATUS "make-lavfi-fixtures: wrote ${dir}")
