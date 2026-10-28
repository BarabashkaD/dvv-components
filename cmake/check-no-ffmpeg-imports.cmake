# Fails when a binary imports an FFmpeg library (M4 design D3: FFmpeg is loaded at run time and never
# linked). Installed with dvv_components; dvv_check_no_ffmpeg_imports() adds a CTest test for it.
#   cmake -DBINARIES=<file>[,<file>...] (-DDUMPBIN=<dumpbin> | -DREADELF=<readelf>)
#         -P check-no-ffmpeg-imports.cmake
#   cmake -DLISTING=<text file> -P check-no-ffmpeg-imports.cmake   (checks a saved listing)
cmake_minimum_required(VERSION 4.2)
set(ffmpeg_libraries avutil avcodec avformat avfilter avdevice swscale swresample postproc)

# check_listing(<what> <dumpbin /dependents or readelf -d output>)
function(check_listing what listing)
    # The leading space lets a name at the very start pass the "not part of a longer name" rule.
    string(TOLOWER " ${listing}" listing)
    foreach(name IN LISTS ffmpeg_libraries)
        # Windows: "avutil-60.dll" or "avutil.dll", never "myavutil.dll" or "my-avutil.dll";
        # Linux: "[libavutil.so.60]".
        if(listing MATCHES "[^a-z0-9_.-]${name}(-[0-9]+)?\\.dll" OR
           listing MATCHES "\\[lib${name}\\.so(\\.[0-9]+)*\\]")
            message(FATAL_ERROR "${what} imports the FFmpeg library ${name}; FFmpeg must be loaded "
                                "at run time, never linked (M4 design D3)")
        endif()
    endforeach()
endfunction()

if(DEFINED LISTING)
    file(READ "${LISTING}" listing)
    check_listing("${LISTING}" "${listing}")
    message(STATUS "no FFmpeg import in ${LISTING}")
    return()
endif()
if(NOT BINARIES)
    message(FATAL_ERROR "pass -DBINARIES=<file>[,<file>...] or -DLISTING=<text file>")
endif()
string(REPLACE "," ";" binaries "${BINARIES}")
foreach(binary IN LISTS binaries)
    if(DUMPBIN)
        execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${binary}"
                        RESULT_VARIABLE status OUTPUT_VARIABLE listing ERROR_VARIABLE error)
    elseif(READELF)
        execute_process(COMMAND "${READELF}" -d "${binary}"
                        RESULT_VARIABLE status OUTPUT_VARIABLE listing ERROR_VARIABLE error)
    else()
        message(FATAL_ERROR "pass -DDUMPBIN=<dumpbin> or -DREADELF=<readelf>")
    endif()
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "reading the imports of ${binary} failed (${status}): ${error}")
    endif()
    check_listing("${binary}" "${listing}")
endforeach()
list(LENGTH binaries count)
message(STATUS "no FFmpeg import in ${count} binaries")
