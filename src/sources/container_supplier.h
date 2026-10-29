#pragma once

// The `ffmpeg` provider's container path (design v2 §4.4, D5, D28; v1.0 codec scope): open with
// the lossless codec allowlist and the frame index, then the indexed and Sequential decode paths on
// the source's worker.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"
#include "sources/av_worker.h"
#include "sources/frame_ring.h"

namespace dvv::components::detail {

// The decode state of one container source; after open it is touched only on its worker.
class ContainerSupplier final : public IFrameSupplier {
public:
    // Open steps 2-8 (design v2 §4.4): avformat_open_input with protocol_whitelist=file and
    // codec_whitelist = LOSSLESS_CODECS (FFmpeg opens no other decoder, also while it probes), and
    // avformat_find_stream_info; the stream choice, the codec allowlist (before any scan or decoder
    // lookup; it also wins over a failed stream probe), the frame index (the container's own when
    // it lists every frame, else a demux-only scan), the mode, the reopen, the decoder and frame 0
    // (into the ring). `stopped` ends the open with Errc::Internal "source closed" (checked before
    // each scanned packet and while frame 0 decodes).
    [[nodiscard]] static Result<std::unique_ptr<ContainerSupplier>> open(
        const std::string& path_utf8, const StopCheck& stopped);
    ~ContainerSupplier() override = default;
    ContainerSupplier(const ContainerSupplier&) = delete;
    ContainerSupplier& operator=(const ContainerSupplier&) = delete;
    ContainerSupplier(ContainerSupplier&&) = delete;
    ContainerSupplier& operator=(ContainerSupplier&&) = delete;

    [[nodiscard]] Result<const AVFrame*> frame(std::int64_t index,
                                               const StopCheck& stopped) override;

    // Valid after open() (on the opening thread, before the supplier moves to its worker):
    [[nodiscard]] const AVFrame& firstFrame() const noexcept;  // frame 0, held by the ring
    [[nodiscard]] std::int64_t frameCount() const noexcept { return frame_count_; }
    [[nodiscard]] bool sequential() const noexcept { return sequential_; }
    // av_guess_frame_rate of the chosen stream (not reduced; may be 0/1).
    [[nodiscard]] AVRational guessedFrameRate() const noexcept;
    [[nodiscard]] const AVCodecParameters& codecParameters() const noexcept;
    // Packets the open-time scan read: 0 when the container's own index listed every frame.
    [[nodiscard]] std::int64_t scannedPackets() const noexcept { return scanned_; }
    // Seeks and restarts since open (a test seam: requests that decode on add none).
    [[nodiscard]] std::int64_t resets() const noexcept { return resets_; }
    // The decoder's thread_count after avcodec_open2 (FFmpeg's choice when it was 0; tests).
    [[nodiscard]] int decoderThreads() const noexcept;

    // Test seam (an injected reopen failure): later reopens open `path_utf8` instead.
    void setPathForTest(const std::string& path_utf8) { url_ = "file:" + path_utf8; }

private:
    struct Entry {
        std::int64_t pts = 0;
        bool key = false;
    };
    // The first frame's size and AVPixelFormat: every later frame must equal them (§5.4).
    struct Expected {
        std::uint32_t width = 0, height = 0;
        int format = -1;
    };

    ContainerSupplier() = default;
    // Open step 4 from the container's index (AVI idx1, MOV sample tables): true, with entries_
    // filled, when it lists every frame of `stream` (as many entries as nb_frames, none discarded,
    // strictly increasing timestamps that start at the stream's start_time). MKV cues and NUT
    // syncpoints list only some frames and have no frame count, so they give false.
    [[nodiscard]] bool indexFromContainer(AVStream& stream);
    // Open step 4 otherwise: reads every packet of the chosen stream (the others are discarded),
    // checking `stopped` before each read.
    [[nodiscard]] Result<void> scanIndex(AVFormatContext& format, const StopCheck& stopped);
    // Close and open the format context again (open step 6; also the Sequential restart). Until it
    // succeeds the next request restarts again, so a failed reopen is an error for every request.
    [[nodiscard]] Result<void> reopen();
    // Sequential: reopen, flush the decoder, empty the ring; numbering starts again at 0.
    [[nodiscard]] Result<void> restart();
    // Indexed: seek to the last keyframe entry at or before `index`, flush, empty the ring.
    [[nodiscard]] Result<void> seekTo(std::int64_t index);
    // Decodes forward until frame `index` is in the ring. A damaged frame (the decoder
    // reported an error for it or for a frame it references) never enters the ring: frame `index`
    // damaged gives Errc::Format "frame <index> is damaged (<reason>)" and keeps the decoder's
    // position, so a request for the next frame decodes on.
    [[nodiscard]] Result<const AVFrame*> decodeUntil(std::int64_t index, const StopCheck& stopped);
    // Reads the next packet of the stream and sends it to the decoder; the drain packet at the
    // demuxer's end.
    [[nodiscard]] Result<void> sendNextPacket(const StopCheck& stopped);
    // The index of a decoded frame (Sequential: the next number; indexed: the entry of its pts),
    // nullopt when it is dropped.
    [[nodiscard]] std::optional<std::int64_t> indexOf(const AVFrame& frame);
    // Creates the ring when the first frame arrives (its capacity needs the decoded format).
    void createRing(const AVFrame& frame);

    std::string url_;  // "file:" + the UTF-8 path
    int stream_ = -1;
    ffmpeg::FormatContextPtr format_;
    // FFV1's codec_->opaque (its damage mark); declared before codec_, so it outlives the decoder.
    ffmpeg::DecodeErrorMark decode_error_;
    ffmpeg::CodecContextPtr codec_;
    ffmpeg::PacketPtr packet_;
    ffmpeg::FramePtr scratch_;  // the decoder's output before it enters the ring
    std::optional<FrameRing> ring_;
    std::vector<Entry> entries_;  // indexed: sorted by pts
    bool sequential_ = false;
    std::int64_t frame_count_ = 0;
    std::int64_t scanned_ = 0;  // see scannedPackets()
    std::int64_t resets_ = 0;   // see resets()
    // The newest index decoded since the last seek or restart; -1 right after a (re)open, nullopt
    // right after a seek.
    std::optional<std::int64_t> last_;
    std::int64_t floor_ = 0;    // indexed: frames below this entry are dropped (open-GOP leaders)
    bool demuxer_end_ = false;  // the drain packet was sent
    // The decoder is drained or stopped by an error, or a reopen failed: the next request seeks or
    // restarts.
    bool needs_reset_ = false;
    // The decoder reported damage since the last key frame: the frames until the next
    // key frame reference the damaged one. Cleared by a seek or restart.
    bool damaged_ = false;
    // The frame the decoder last reported damage for (nullopt when it was dropped); for messages.
    std::optional<std::int64_t> damaged_from_;
    // Set by decodeUntil when frame `index` was damaged: the decoder is still in step, so frame()
    // does not reset.
    bool kept_position_ = false;
    std::optional<Expected> expected_;
};

// The result of open steps 2-12 for one `ffmpeg` URI: the source's info and its worker (which owns
// the supplier, with frame 0 in its ring).
struct OpenedContainer {
    media::SourceInfo info;
    std::unique_ptr<AvWorker> worker;
};
// FfmpegSourceProvider::open's work (it runs on the I/O pool): the supplier, the catalogue format,
// the frame rate, the D28 query check, the colour, the resolved URI and the worker.
[[nodiscard]] Result<OpenedContainer> openContainer(const Uri& uri);

// Test seam (stop test): the worker of a file opened with the same code as open (no query).
[[nodiscard]] Result<std::unique_ptr<AvWorker>> makeContainerWorkerForTest(
    const std::filesystem::path& path);

// Test seam (the single-thread TSan case): the decoder thread_count of every later open; 0 (the
// default) lets FFmpeg choose, 1 starts no FFmpeg threads. Thread-safe.
void setDecoderThreadsForTest(int threads) noexcept;

}  // namespace dvv::components::detail
