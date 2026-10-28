#pragma once

// The only include of FFmpeg's C headers (M4 design D3): they have no extern "C" of their own. The
// include directory is a system directory (dvv::ffmpeg_headers), so their warnings are not ours.
// Never use a C-only macro or an inline helper that calls an exported function (it would need
// FFmpeg linked): AVRational{1, AV_TIME_BASE} instead of AV_TIME_BASE_Q, api().av_strerror instead
// of av_err2str. A violation shows up as an unresolved symbol when linking.

#include <cstdarg>
#include <cstdint>  // UINT64_C and friends, which FFmpeg's common.h requires in C++

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
}
