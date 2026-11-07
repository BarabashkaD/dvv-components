# Self-test of check-no-ffmpeg-imports.cmake on saved listings (dumpbin /dependents and readelf -d
# output): clean listings pass, a listing naming an FFmpeg library fails with that name.
# Called by the dvv_components.no_ffmpeg_imports_selftest test with -DSCRIPT and -DWORK_DIR.
cmake_minimum_required(VERSION 4.2)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# expect(<name> <pass|library name> <listing>)
function(expect name expected listing)
    set(file "${WORK_DIR}/${name}.txt")
    file(WRITE "${file}" "${listing}")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DLISTING=${file}" -P "${SCRIPT}"
                    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(expected STREQUAL "pass")
        if(NOT status EQUAL 0 OR NOT output MATCHES "no FFmpeg import")
            message(FATAL_ERROR "${name}: expected a pass, got (${status}): ${output}")
        endif()
    elseif(status EQUAL 0 OR NOT output MATCHES "imports the FFmpeg library ${expected}")
        message(FATAL_ERROR "${name}: expected a failure naming ${expected}, got (${status}): "
                            "${output}")
    endif()
    message(STATUS "${name}: ok")
endfunction()

set(windows_header "Dump of file consumer.exe\n\nFile Type: EXECUTABLE IMAGE\n\n  Image has the following dependencies:\n\n")
expect(windows-clean pass
       "${windows_header}    KERNEL32.dll\n    MSVCP140.dll\n    myavutil.dll\n    my-avutil.dll\n    x.avcodec.dll\n    libavutil_extra.so\n\n  Summary\n")
expect(windows-avcodec avcodec "${windows_header}    KERNEL32.dll\n    avcodec-62.dll\n")
expect(windows-avfilter avfilter "${windows_header}    AVFILTER.DLL\n    KERNEL32.dll\n")
expect(windows-postproc postproc "${windows_header}    postproc-58.dll\n")

set(linux_header "Dynamic section at offset 0x2d50 contains 28 entries:\n  Tag        Type                         Name/Value\n")
expect(linux-clean pass
       "${linux_header} 0x0000000000000001 (NEEDED)  Shared library: [libstdc++.so.6]\n 0x000000000000001d (RUNPATH)  Library runpath: [$ORIGIN]\n")
expect(linux-swresample swresample
       "${linux_header} 0x0000000000000001 (NEEDED) Shared library: [libswresample.so.6]\n")
expect(linux-avdevice avdevice
       "${linux_header} 0x0000000000000001 (NEEDED) Shared library: [libavdevice.so]\n")

set(macos_header "consumer:\n")
expect(macos-clean pass
       "${macos_header}\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 2000.0.0)\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1400.0.0)\n\t/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation (compatibility version 300.0.0)\n\t@rpath/libavutil_extra.dylib (compatibility version 1.0.0)\n")
expect(macos-avcodec avcodec
       "${macos_header}\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n\t@rpath/libavcodec.62.dylib (compatibility version 62.0.0)\n")
expect(macos-avutil-path avutil
       "${macos_header}\t/opt/ffmpeg/lib/libavutil.60.26.103.dylib (compatibility version 60.0.0)\n")
expect(macos-unversioned swscale
       "${macos_header}\t@loader_path/libswscale.dylib (compatibility version 1.0.0)\n")

message(STATUS "import check self-test passed")
