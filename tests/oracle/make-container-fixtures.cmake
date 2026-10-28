# The container fixtures of the ffmpeg provider tests (M4 Task 5a; v1.0 codec scope: lossless
# codecs only, except the lossy clip that only has to be refused), made by the prefix's ffmpeg.
#
#   cmake -DFFMPEG=<ffmpeg program> -DOUT=<dir> [-DLIBDIR=<FFmpeg library dir>] -P <this file>
#
# Writes into <OUT>/<version>/ (<version>: the third token of `ffmpeg -version` line 1, every
# character outside [A-Za-z0-9._-] removed) and records that absolute folder in <OUT>/current.txt;
# a done.stamp there written by the same script skips the work. Without the program: prints "ffmpeg program missing"
# and exits 0 without a stamp. Outside Windows every command runs with LD_LIBRARY_PATH=<LIBDIR>.
cmake_minimum_required(VERSION 4.2)

if(NOT OUT)
    message(FATAL_ERROR "make-container-fixtures: OUT is required")
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
    message(FATAL_ERROR "make-container-fixtures: ffmpeg -version failed (${result}):\n${version_text}")
endif()
string(REGEX MATCH "^[^\n]*" first_line "${version_text}")
string(REGEX REPLACE " +" ";" tokens "${first_line}")
list(LENGTH tokens token_count)
if(token_count LESS 3)
    message(FATAL_ERROR "make-container-fixtures: unexpected ffmpeg -version line '${first_line}'")
endif()
list(GET tokens 2 version)
string(REGEX REPLACE "[^A-Za-z0-9._-]" "" version "${version}")
if(version STREQUAL "")
    message(FATAL_ERROR "make-container-fixtures: no version in '${first_line}'")
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
        message(STATUS "make-container-fixtures: ${dir} is complete")
        return()
    endif()
endif()
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")

# ffmpeg -hide_banner -nostdin -y <arguments...> in the fixture folder; a failure is fatal.
function(run_ffmpeg)
    execute_process(COMMAND ${command_prefix} "${FFMPEG}" -hide_banner -nostdin -y ${ARGN}
                    WORKING_DIRECTORY "${dir}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " arguments)
        message(FATAL_ERROR "make-container-fixtures: ffmpeg ${arguments} failed (${result}):\n"
                            "${output}")
    endif()
endfunction()

# The framemd5 reference of a file's first video stream: <stem>.framemd5.
function(reference file)
    cmake_path(GET file STEM stem)
    run_ffmpeg(-i "${file}" -map 0:v:0 -fps_mode passthrough -f framemd5 "${stem}.framemd5")
endfunction()

# FFV1 in MKV, one file per catalogue format FFV1 encodes (gbrp through ffvhuff: FFV1 lists no
# 8-bit gbrp and would pick another format; the name keeps the list one loop).
set(ffv1_formats gray gray10le gray12le gray16le yuv420p yuv422p yuv444p yuv420p10le yuv422p10le
                 yuv444p10le yuv420p12le yuv422p12le yuv444p12le yuv420p16le yuv444p16le gbrp
                 gbrp10le gbrp16le)
foreach(format IN LISTS ffv1_formats)
    set(codec -c:v ffv1 -level 3)
    if(format STREQUAL "gbrp")
        set(codec -c:v ffvhuff)
    endif()
    set(colour "")
    if(format STREQUAL "yuv420p")
        # The output options alone leave trc and primaries unknown with this ffmpeg; setparams tags
        # the frames themselves.
        set(colour -vf "setparams=range=tv:colorspace=bt709:color_trc=bt709:color_primaries=bt709"
                   -colorspace bt709 -color_range tv -color_trc bt709 -color_primaries bt709)
    endif()
    run_ffmpeg(-f lavfi -i testsrc2=s=352x288:r=25 -frames:v 10 -pix_fmt ${format} ${codec}
               ${colour} ffv1_${format}.mkv)
    reference(ffv1_${format}.mkv)
endforeach()

# The Sequential fixture (D5; Task 0 chose seq.mkv: FFV1 in MKV with pairs of equal pts) and its
# long variant for the stop tests (no reference). The stop tests pace the decoding themselves, so
# long.mkv needs only its 3000 frames, not a large frame size.
foreach(name_size_frames IN ITEMS seq:352x288:30 long:352x288:3000)
    string(REPLACE ":" ";" name_size_frames "${name_size_frames}")
    list(GET name_size_frames 0 name)
    list(GET name_size_frames 1 size)
    list(GET name_size_frames 2 frames)
    run_ffmpeg(-f lavfi -i testsrc2=s=${size}:r=25 -frames:v ${frames}
               -vf "setpts=floor(N/2)/(25*TB)" -fps_mode passthrough -pix_fmt yuv420p
               -c:v ffv1 -level 3 ${name}.mkv)
endforeach()
reference(seq.mkv)

# FFV1 with a key frame every 3 frames (0, 3, 6, 9): the damaged-file tests check that
# the damage ends at the next key frame.
run_ffmpeg(-f lavfi -i testsrc2=s=352x288:r=25 -frames:v 10 -pix_fmt yuv420p -c:v ffv1 -level 3
           -g 3 ffv1_gop.mkv)
reference(ffv1_gop.mkv)

# Lossless codecs per container (v1.0 codec scope).
foreach(entry IN ITEMS "yuv422p;huffyuv;lossless_huffyuv.avi" "yuv420p;utvideo;lossless_utvideo.mkv"
                       "yuv422p10le;v210;lossless_v210.mov" "yuv444p;magicyuv;lossless_magicyuv.nut")
    list(GET entry 0 format)
    list(GET entry 1 codec)
    list(GET entry 2 file)
    run_ffmpeg(-f lavfi -i testsrc2=s=192x144:r=25 -frames:v 10 -pix_fmt ${format} -c:v ${codec}
               ${file})
    reference(${file})
endforeach()

# The other allowlisted codecs (Task 11-pre): rawvideo in AVI, MOV and NUT
# (the stem names the container, so the references do not collide), r210 and qtrle in MOV (qtrle
# keeps inter frames, so its reads exercise the seek to a key frame).
foreach(entry IN ITEMS "yuv420p;rawvideo;lossless_rawvideo_avi.avi"
                       "uyvy422;rawvideo;lossless_rawvideo_mov.mov"
                       "yuv444p;rawvideo;lossless_rawvideo_nut.nut"
                       "gbrp10le;r210;lossless_r210.mov" "rgb24;qtrle;lossless_qtrle.mov")
    list(GET entry 0 format)
    list(GET entry 1 codec)
    list(GET entry 2 file)
    run_ffmpeg(-f lavfi -i testsrc2=s=192x144:r=25 -frames:v 10 -pix_fmt ${format} -c:v ${codec}
               ${file})
    reference(${file})
endforeach()
# v410 in MOV: FFmpeg 8 decodes it as rawvideo with output v30xle, which is not in the catalogue,
# so open must refuse it cleanly; no reference.
run_ffmpeg(-f lavfi -i testsrc2=s=192x144:r=25 -frames:v 10 -pix_fmt yuv444p10le -c:v v410
           lossless_v410.mov)

# A frame size change inside one stream (§5.4): PNG carries its size in every frame.
run_ffmpeg(-f lavfi -i testsrc2=s=352x288:r=25 -frames:v 5 -pix_fmt gray -c:v png part_a.mkv)
run_ffmpeg(-f lavfi -i testsrc2=s=176x144:r=25 -frames:v 5 -pix_fmt gray -c:v png part_b.mkv)
file(WRITE "${dir}/concat.txt" "file 'part_a.mkv'\nfile 'part_b.mkv'\n")
run_ffmpeg(-f concat -safe 0 -i concat.txt -c copy concat_sizes.mkv)

# The lossy refusal clip (Task 0 recorded "mpeg4 encoder: yes"); no reference.
run_ffmpeg(-f lavfi -i testsrc2=s=64x48:r=25 -frames:v 5 -pix_fmt yuv420p -c:v mpeg4
           lossy_mpeg4.mp4)

# A file without a video stream.
run_ffmpeg(-f lavfi -i sine=frequency=440:duration=1 -c:a pcm_s16le audio_only.mka)

file(WRITE "${dir}/done.stamp" "${stamp}")
message(STATUS "make-container-fixtures: wrote ${dir}")
