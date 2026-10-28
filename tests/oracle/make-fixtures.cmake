# The FFmpeg oracle fixtures (M4 design v2 §8.3, Task 6): a testsrc2 clip per catalogue format, the
# extractplanes reference planes of each clip and the swscale rgb24 outputs of the display rows, all
# made by the prefix's ffmpeg program.
#
#   cmake -DFFMPEG=<ffmpeg program> -DOUT=<dir> [-DLIBDIR=<FFmpeg library dir>] -P <this file>
#
# Writes into <OUT>/<version>/ (<version>: the third token of `ffmpeg -version` line 1, every
# character outside [A-Za-z0-9._-] removed) and records that absolute folder in <OUT>/current.txt;
# a done.stamp there written by the same script skips the work. Without the program: prints "ffmpeg
# program missing; oracle fixtures not made" and exits 0 without a stamp. Outside Windows every
# command runs with LD_LIBRARY_PATH=<LIBDIR>. A failing command is fatal, except the clip of a format
# the program cannot write: that format is logged and left out of the manifest.
#
# Output:
#   clip_<F>.raw              10 frames of testsrc2 in format F (v210: the v210 encoder's output);
#                             deeper than 8 bits: testsrc2 plus a geq pattern in the bits below the
#                             top 8 (see deep_formats)
#   <F>_<c>.raw               component c of every frame (extractplanes, one run per letter)
#   display_<F>_<M>_<R>.rgb   rgb24 of swscale (accurate_rnd+full_chroma_int+bitexact)
#   display_<F>_full.rgb      the same for gray and RGB formats (full range only)
#   manifest.txt              "unpack|<F>|<W>|<H>|<frames>|<clip>|<letters>" per covered format and
#                             "display|<F>|<M or ->|<limited or full>|<rgb file>" per display row
cmake_minimum_required(VERSION 4.2)

if(NOT OUT)
    message(FATAL_ERROR "make-fixtures: OUT is required")
endif()
if(NOT FFMPEG OR NOT EXISTS "${FFMPEG}")
    message("ffmpeg program missing; oracle fixtures not made")
    # Fixtures of an earlier run must not keep the oracle green without the program.
    file(REMOVE "${OUT}/current.txt")
    return()
endif()

set(command_prefix "")
if(NOT CMAKE_HOST_WIN32 AND LIBDIR)
    set(command_prefix "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${LIBDIR}")
endif()

execute_process(COMMAND ${command_prefix} "${FFMPEG}" -hide_banner -version
                RESULT_VARIABLE result OUTPUT_VARIABLE version_text ERROR_VARIABLE version_text)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "make-fixtures: ffmpeg -version failed (${result}):\n${version_text}")
endif()
string(REGEX MATCH "^[^\n]*" first_line "${version_text}")
string(REGEX REPLACE " +" ";" tokens "${first_line}")
list(LENGTH tokens token_count)
if(token_count LESS 3)
    message(FATAL_ERROR "make-fixtures: unexpected ffmpeg -version line '${first_line}'")
endif()
list(GET tokens 2 version)
string(REGEX REPLACE "[^A-Za-z0-9._-]" "" version "${version}")
if(version STREQUAL "")
    message(FATAL_ERROR "make-fixtures: no version in '${first_line}'")
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
        message(STATUS "make-fixtures: ${dir} is complete")
        return()
    endif()
endif()
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")

set(frames 10)
set(width 352)
set(height 288)
set(v210_width 384)
set(ffmpeg "${FFMPEG}" -hide_banner -nostdin -y)

# Runs ffmpeg with the given arguments in the fixture folder; fatal on failure unless OK_VAR is
# given, in which case it receives TRUE or FALSE and LOG_VAR the output.
function(run_ffmpeg)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "OK_VAR;LOG_VAR" "ARGS")
    execute_process(COMMAND ${command_prefix} ${ffmpeg} ${arg_ARGS}
                    WORKING_DIRECTORY "${dir}"
                    RESULT_VARIABLE run_result OUTPUT_VARIABLE run_output ERROR_VARIABLE run_output)
    if(arg_OK_VAR)
        if(run_result EQUAL 0)
            set(${arg_OK_VAR} TRUE PARENT_SCOPE)
        else()
            set(${arg_OK_VAR} FALSE PARENT_SCOPE)
        endif()
        set(${arg_LOG_VAR} "${run_output}" PARENT_SCOPE)
    elseif(NOT run_result EQUAL 0)
        list(JOIN arg_ARGS " " joined)
        message(FATAL_ERROR "make-fixtures: ffmpeg ${joined} failed (${run_result}):\n${run_output}")
    endif()
endfunction()

# The per-format reference table (binding): "<formats>|<step>|<letters>", where
# the step is the lossless same-depth planar conversion before extractplanes ("-": none).
set(table
    "gray gray10le gray12le gray16le|-|y"
    "gray16be|format=gray16le|y"
    "yuv420p yuv422p yuv444p yuv420p10le yuv422p10le yuv444p10le yuv420p12le yuv422p12le yuv444p12le yuv420p16le yuv444p16le|-|yuv"
    "yuv420p10be|format=yuv420p10le|yuv"
    "nv12 nv21|format=yuv420p|yuv"
    "p010le|format=yuv420p10le|yuv"
    "p016le|format=yuv420p16le|yuv"
    "yuyv422 uyvy422|format=yuv422p|yuv"
    "y210le|format=yuv422p10le|yuv"
    "v210|-|yuv"
    "rgb24 bgr24|format=gbrp|rgb"
    "rgba bgra argb|format=gbrap|rgba"
    "rgb48le rgb48be|format=gbrp16le|rgb"
    "rgba64le|format=gbrap16le|rgba"
    "x2rgb10le|format=gbrp10le|rgb"
    "gbrp gbrp10le gbrp16le|-|rgb")

# Formats deeper than 8 bits. testsrc2 draws 8-bit colours shifted to the depth,
# so every bit below the top 8 would be zero and the oracle could not see a broken low bit. For these
# formats geq first adds a moving pattern of 0..255 to a 16-bit planar copy (alpha: 65535 minus the
# pattern), which leaves non-zero bits below the top 8 in every component after the lossless clip
# writer's conversion to the format. The oracle test checks that each such reference plane has them.
set(deep_formats
    gray10le gray12le gray16le gray16be yuv420p10le yuv422p10le yuv444p10le yuv420p12le yuv422p12le
    yuv444p12le yuv420p16le yuv444p16le yuv420p10be p010le p016le y210le v210 rgb48le rgb48be
    rgba64le x2rgb10le gbrp10le gbrp16le)
set(pattern_1 "mod(X+3*Y+7*N,256)")
set(pattern_2 "mod(5*X+Y+11*N,256)")
set(pattern_3 "mod(X+7*Y+13*N,256)")

# The lavfi source of a clip of <width> pixels: plain testsrc2, or for a deep format testsrc2 with the
# geq pattern in the 16-bit planar format of its model (letters: y, yuv, rgb or rgba).
function(clip_source format letters width out_var)
    set(source "testsrc2=s=${width}x${height}:r=25")
    if(format IN_LIST deep_formats)
        set(add_1 "+${pattern_1},0,65535)'")
        set(add_2 "+${pattern_2},0,65535)'")
        set(add_3 "+${pattern_3},0,65535)'")
        if(letters STREQUAL "y")
            string(APPEND source ",format=gray16le,geq=lum='clip(lum(X,Y)${add_1}")
        elseif(letters STREQUAL "yuv")
            string(APPEND source ",format=yuv444p16le,geq=lum='clip(lum(X,Y)${add_1}"
                                 ":cb='clip(cb(X,Y)${add_2}:cr='clip(cr(X,Y)${add_3}")
        elseif(letters STREQUAL "rgb" OR letters STREQUAL "rgba")
            set(planar gbrp16le)
            set(alpha "")
            if(letters STREQUAL "rgba")
                set(planar gbrap16le)
                set(alpha ":a='65535-${pattern_1}'")
            endif()
            string(APPEND source ",format=${planar},geq=r='clip(r(X,Y)${add_1}"
                                 ":g='clip(g(X,Y)${add_2}:b='clip(b(X,Y)${add_3}${alpha}")
        else()
            message(FATAL_ERROR "make-fixtures: no deep source for ${format} (${letters})")
        endif()
    endif()
    set(${out_var} "${source}" PARENT_SCOPE)
endfunction()

set(manifest "")
foreach(row IN LISTS table)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 formats)
    list(GET fields 1 step)
    list(GET fields 2 letters)
    string(REPLACE " " ";" formats "${formats}")
    foreach(format IN LISTS formats)
        set(clip "clip_${format}.raw")
        if(format STREQUAL "v210")
            set(w ${v210_width})
            clip_source(${format} ${letters} ${w} source)
            run_ffmpeg(ARGS -f lavfi -i "${source}" -frames:v ${frames}
                            -pix_fmt yuv422p10le -c:v v210 -f rawvideo ${clip}
                       OK_VAR clip_ok LOG_VAR clip_log)
            set(input -f v210 -video_size ${w}x${height} -framerate 25 -i ${clip})
        else()
            set(w ${width})
            clip_source(${format} ${letters} ${w} source)
            run_ffmpeg(ARGS -f lavfi -i "${source}" -frames:v ${frames}
                            -pix_fmt ${format} -sws_flags accurate_rnd+bitexact
                            -fps_mode passthrough -f rawvideo ${clip}
                       OK_VAR clip_ok LOG_VAR clip_log)
            set(input -f rawvideo -pix_fmt ${format} -video_size ${w}x${height} -framerate 25
                      -i ${clip})
        endif()
        if(NOT clip_ok)
            message("make-fixtures: not covered: ffmpeg cannot write ${format}:\n${clip_log}")
            continue()
        endif()
        set(prefix "")
        if(NOT step STREQUAL "-")
            set(prefix "${step},")
        endif()
        string(LENGTH "${letters}" letter_count)
        math(EXPR last "${letter_count} - 1")
        foreach(i RANGE ${last})
            string(SUBSTRING "${letters}" ${i} 1 letter)
            run_ffmpeg(ARGS ${input} -sws_flags accurate_rnd+bitexact
                            -vf "${prefix}extractplanes=${letter}" -fps_mode passthrough
                            -f rawvideo ${format}_${letter}.raw)
        endforeach()
        string(APPEND manifest "unpack|${format}|${w}|${height}|${frames}|${clip}|${letters}\n")
    endforeach()
endforeach()

# The display rows (§8.3): transfer sRGB, primaries BT.709.
set(scale_flags "accurate_rnd+full_chroma_int+bitexact")
foreach(format IN ITEMS yuv444p yuv444p10le)
    if(NOT EXISTS "${dir}/clip_${format}.raw")
        continue()
    endif()
    foreach(matrix IN ITEMS bt601 bt709)
        foreach(range IN ITEMS limited full)
            if(range STREQUAL "limited")
                set(in_range tv)
            else()
                set(in_range pc)
            endif()
            set(output "display_${format}_${matrix}_${range}.rgb")
            run_ffmpeg(ARGS -f rawvideo -pix_fmt ${format} -video_size ${width}x${height}
                            -framerate 25 -i clip_${format}.raw
                            -vf "scale=in_color_matrix=${matrix}:in_range=${in_range}:out_range=pc:flags=${scale_flags},format=rgb24"
                            -fps_mode passthrough -f rawvideo ${output})
            string(APPEND manifest "display|${format}|${matrix}|${range}|${output}\n")
        endforeach()
    endforeach()
endforeach()
foreach(format IN ITEMS gray gray10le gbrp)
    if(NOT EXISTS "${dir}/clip_${format}.raw")
        continue()
    endif()
    set(output "display_${format}_full.rgb")
    run_ffmpeg(ARGS -f rawvideo -pix_fmt ${format} -video_size ${width}x${height} -framerate 25
                    -i clip_${format}.raw
                    -vf "scale=in_range=pc:out_range=pc:flags=${scale_flags},format=rgb24"
                    -fps_mode passthrough -f rawvideo ${output})
    string(APPEND manifest "display|${format}|-|full|${output}\n")
endforeach()

file(WRITE "${dir}/manifest.txt" "${manifest}")
file(WRITE "${dir}/done.stamp" "${stamp}")
message(STATUS "make-fixtures: wrote ${dir}")
