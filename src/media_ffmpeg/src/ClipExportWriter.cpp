#include "dvs/media/ClipExportWriter.h"

#include "AvRaii.h"
#include "ClipExportWriterTestHooks.h"
#include "dvs/platform/AtomicFilePublisher.h"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

std::atomic<testing::ClipExportCloseCallback> closeOutputCallback{&avio_closep};

struct CancelState final {
    const std::atomic_bool* cancelRequested = nullptr;
};

[[nodiscard]] int interruptCallback(void* opaque) noexcept {
    const auto* const state = static_cast<const CancelState*>(opaque);
    if (state == nullptr || state->cancelRequested == nullptr) {
        return 0;
    }
    return state->cancelRequested->load(std::memory_order_acquire) ? 1 : 0;
}

[[nodiscard]] bool isCanceled(const std::atomic_bool& flag) noexcept {
    return flag.load(std::memory_order_acquire);
}

[[nodiscard]] std::string ffmpegError(const int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "FFmpeg returned error " + std::to_string(errorCode) + ".";
    }
    return std::string{buffer};
}

[[nodiscard]] std::string narrow(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] application::ClipExportReport makeReport(const application::ClipExportJob& job,
                                                       const application::ClipExportOutcome outcome,
                                                       const std::int64_t packetsWritten,
                                                       std::string detail) {
    application::ClipExportReport report;
    report.requestId = job.requestId;
    report.outcome = outcome;
    report.packetsWritten = packetsWritten;
    report.technicalDetail = std::move(detail);
    return report;
}

[[nodiscard]] bool muxerSupportsFaststart(const char* const muxerName) noexcept {
    if (muxerName == nullptr) {
        return false;
    }
    const std::string_view name{muxerName};
    return name == "mov" || name == "mp4" || name == "3gp" || name == "3g2" || name == "ipod";
}

// A demuxed source with the video stream already located. `error` is empty when the input is
// usable, and the context is null in every failing case.
struct OpenedInput final {
    internal::AvFormatContextPtr context;
    AVStream* videoStream = nullptr;
    int videoStreamIndex = -1;
    std::int64_t presentationOrigin = 0;
    std::string error;
};

// AVStream::start_time is the first presentation timestamp, not the first decode timestamp.
// Some demuxers cannot provide it. Match playback's display-order origin in that case by taking
// the minimum video PTS, then rewind. This remains a demux-only operation on the export worker.
[[nodiscard]] std::optional<std::int64_t>
presentationOrigin(AVFormatContext* const input,
                   const int streamIndex,
                   const std::atomic_bool& cancelRequested) {
    const AVStream& stream = *input->streams[streamIndex];
    if (stream.start_time != AV_NOPTS_VALUE) {
        return stream.start_time;
    }

    internal::AvPacketPtr packet{av_packet_alloc()};
    if (!packet) {
        return std::nullopt;
    }
    std::optional<std::int64_t> first;
    while (!isCanceled(cancelRequested)) {
        const int result = av_read_frame(input, packet.get());
        if (result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            return std::nullopt;
        }
        if (packet->stream_index == streamIndex && packet->pts != AV_NOPTS_VALUE) {
            first = first.has_value() ? std::min(*first, packet->pts) : packet->pts;
        }
        av_packet_unref(packet.get());
    }
    if (isCanceled(cancelRequested) || !first.has_value() ||
        av_seek_frame(input, streamIndex, *first, AVSEEK_FLAG_BACKWARD) < 0) {
        return std::nullopt;
    }
    return first;
}

[[nodiscard]] OpenedInput openVideoInput(const std::filesystem::path& sourcePath,
                                         const CancelState& cancelState) {
    OpenedInput opened;

    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(sourcePath, filesystemError)) {
        opened.error = "The source file is not readable: " + narrow(sourcePath);
        return opened;
    }

    const std::string sourceUrl = narrow(sourcePath);
    AVFormatContext* rawInput = avformat_alloc_context();
    if (rawInput == nullptr) {
        opened.error = "FFmpeg could not allocate an input format context.";
        return opened;
    }
    rawInput->interrupt_callback.callback = interruptCallback;
    rawInput->interrupt_callback.opaque = const_cast<CancelState*>(&cancelState);

    const int openResult = avformat_open_input(&rawInput, sourceUrl.c_str(), nullptr, nullptr);
    if (openResult < 0) {
        if (rawInput != nullptr) {
            avformat_close_input(&rawInput);
        }
        opened.error = "FFmpeg could not open the source: " + ffmpegError(openResult);
        return opened;
    }
    opened.context.reset(rawInput);

    const int streamInfoResult = avformat_find_stream_info(opened.context.get(), nullptr);
    if (streamInfoResult < 0) {
        opened.error =
            "FFmpeg could not read the source stream information: " + ffmpegError(streamInfoResult);
        return opened;
    }

    const int streamIndex =
        av_find_best_stream(opened.context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0 || static_cast<unsigned int>(streamIndex) >= opened.context->nb_streams) {
        opened.error = "The source has no readable video stream.";
        return opened;
    }
    AVStream* const stream = opened.context->streams[streamIndex];
    if (stream == nullptr || stream->codecpar == nullptr) {
        opened.error = "The selected video stream carries no codec parameters.";
        return opened;
    }
    const auto origin =
        presentationOrigin(opened.context.get(), streamIndex, *cancelState.cancelRequested);
    if (!origin.has_value()) {
        opened.error = "FFmpeg could not establish the video presentation-time origin.";
        opened.context.reset();
        return opened;
    }
    opened.presentationOrigin = *origin;
    opened.videoStream = stream;
    opened.videoStreamIndex = streamIndex;
    return opened;
}

constexpr AVRational kMicrosecondsBase{1, AV_TIME_BASE};

// Everything a stream copy accumulates while it walks the source: the output, which can only be
// created once a keyframe has been accepted, and the numbers the report is built from.
struct ClipCopyState final {
    internal::AvOutputFormatContextPtr output;
    AVStream* outputStream = nullptr;
    AVStream* sourceStream = nullptr;
    int sourceStreamIndex = -1;
    const std::filesystem::path* workPath = nullptr;
    const std::filesystem::path* targetPath = nullptr;
    const CancelState* cancelState = nullptr;
    std::int64_t packetsWritten = 0;
    std::int64_t firstPresentationMicroseconds = 0;
    std::int64_t timestampBase = 0;
    std::int64_t presentationOrigin = 0;
    bool headerWritten = false;
    std::string failureDetail;
};

// Writes one packet into the clip, creating the output lazily around the first keyframe it is
// given. Returns false only when the write itself failed, leaving the reason in
// `state.failureDetail`; a packet that cannot start the copy (a non-keyframe seen before any
// keyframe) is dropped and reported as success.
[[nodiscard]] bool writeVideoPacket(ClipCopyState& state, AVPacket* const packet) {
    if (!state.headerWritten) {
        // A stream copy cannot start mid-GOP; the first accepted packet must be a keyframe.
        if ((packet->flags & AV_PKT_FLAG_KEY) == 0) {
            return true;
        }
        AVFormatContext* rawOutput = nullptr;
        const std::string workUrl = narrow(*state.workPath);
        const std::string targetUrl = narrow(*state.targetPath);
        const AVOutputFormat* const format = av_guess_format(nullptr, targetUrl.c_str(), nullptr);
        if (format == nullptr) {
            state.failureDetail = "FFmpeg could not pick an output container for " + targetUrl;
            return false;
        }
        const int allocateResult =
            avformat_alloc_output_context2(&rawOutput, format, nullptr, workUrl.c_str());
        if (allocateResult < 0 || rawOutput == nullptr) {
            state.failureDetail = "FFmpeg could not pick an output container for " + workUrl +
                                  ": " + ffmpegError(allocateResult);
            return false;
        }
        state.output.reset(rawOutput);
        state.outputStream = avformat_new_stream(state.output.get(), nullptr);
        if (state.outputStream == nullptr) {
            state.failureDetail = "FFmpeg could not create the output video stream.";
            return false;
        }
        const int copyResult =
            avcodec_parameters_copy(state.outputStream->codecpar, state.sourceStream->codecpar);
        if (copyResult < 0) {
            state.failureDetail =
                "FFmpeg could not copy the video codec parameters: " + ffmpegError(copyResult);
            return false;
        }
        state.outputStream->time_base = state.sourceStream->time_base;
        state.outputStream->avg_frame_rate = state.sourceStream->avg_frame_rate;
        state.outputStream->r_frame_rate = state.sourceStream->r_frame_rate;
        // The codec tag describes the source container; leaving it set makes the muxer emit a tag
        // it may not match.
        state.outputStream->codecpar->codec_tag = 0;
        state.output->interrupt_callback.callback = interruptCallback;
        state.output->interrupt_callback.opaque = const_cast<CancelState*>(state.cancelState);

        const int ioResult =
            avio_open2(&state.output->pb, workUrl.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ioResult < 0) {
            state.failureDetail =
                "FFmpeg could not open the clip for writing: " + ffmpegError(ioResult);
            return false;
        }

        AVDictionary* muxerOptions = nullptr;
        if (muxerSupportsFaststart(state.output->oformat != nullptr ? state.output->oformat->name
                                                                    : nullptr)) {
            av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
        }
        const int headerResult = avformat_write_header(state.output.get(), &muxerOptions);
        av_dict_free(&muxerOptions);
        if (headerResult < 0) {
            state.failureDetail =
                "FFmpeg could not write the clip header: " + ffmpegError(headerResult);
            return false;
        }
        state.headerWritten = true;
        state.timestampBase = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (packet->pts != AV_NOPTS_VALUE) {
            state.firstPresentationMicroseconds =
                av_rescale_q(packet->pts - state.presentationOrigin,
                             state.sourceStream->time_base,
                             kMicrosecondsBase);
        }
    }

    // The clip starts at zero, so the first accepted packet's decode timestamp becomes the origin.
    if (packet->pts != AV_NOPTS_VALUE) {
        packet->pts -= state.timestampBase;
    }
    if (packet->dts != AV_NOPTS_VALUE) {
        packet->dts -= state.timestampBase;
    }
    packet->pos = -1;
    packet->stream_index = state.outputStream->index;
    av_packet_rescale_ts(packet, state.sourceStream->time_base, state.outputStream->time_base);

    const int writeResult = av_interleaved_write_frame(state.output.get(), packet);
    if (writeResult < 0) {
        state.failureDetail = "FFmpeg could not write a clip packet: " + ffmpegError(writeResult);
        return false;
    }
    ++state.packetsWritten;
    return true;
}

} // namespace

std::vector<std::int64_t> ClipExportWriter::keyframeTimes(const std::filesystem::path& sourcePath,
                                                          const std::atomic_bool& cancelRequested) {
    CancelState cancelState{.cancelRequested = &cancelRequested};
    OpenedInput opened = openVideoInput(sourcePath, cancelState);
    if (!opened.context) {
        return {};
    }
    AVStream& stream = *opened.videoStream;
    std::vector<std::int64_t> times;

    internal::AvPacketPtr packet{av_packet_alloc()};
    if (!packet) {
        return {};
    }

    const int indexEntryCount = avformat_index_get_entries_count(&stream);
    if (indexEntryCount > 0) {
        // Index timestamps are demuxer-specific: MP4 stores DTS, but its seek accepts PTS.
        // A backward seek can therefore land one GOP before the indexed keyframe. Resolve the
        // entry by walking forward to its byte position, then read the keyframe's actual PTS.
        // Cluster-based indexes can point before the packet, so accept positions at or after it.
        std::int64_t previousPosition = -1;
        bool scanToEnd = false;
        // Some demuxers (for example Matroska) load the remaining cue entries on the first
        // seek. Re-read the count so the initial partial index cannot hide later keyframes.
        for (int index = 0; index < avformat_index_get_entries_count(&stream) && !scanToEnd;
             ++index) {
            if (isCanceled(cancelRequested)) {
                return {};
            }
            const AVIndexEntry* const entry = avformat_index_get_entry(&stream, index);
            if (entry == nullptr) {
                return {};
            }
            if ((entry->flags & AVINDEX_KEYFRAME) == 0) {
                continue;
            }
            // Seeking/reading can grow the demuxer's index and invalidate the entry pointer.
            const std::int64_t indexedPosition = entry->pos;
            const std::int64_t seekTimestamp =
                std::max(entry->timestamp, opened.presentationOrigin);
            const int seekResult = av_seek_frame(
                opened.context.get(), opened.videoStreamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD);
            if (seekResult < 0) {
                return {};
            }

            while (!isCanceled(cancelRequested)) {
                const int readResult = av_read_frame(opened.context.get(), packet.get());
                if (readResult < 0) {
                    if (scanToEnd && readResult == AVERROR_EOF) {
                        break;
                    }
                    // A failed lookup must not turn an index DTS into a made-up presentation time.
                    return {};
                }
                if (packet->stream_index == opened.videoStreamIndex && packet->pos >= 0 &&
                    packet->pos < previousPosition) {
                    // An imprecise seek can revisit older GOPs. Finish with one forward scan
                    // instead of repeatedly rescanning the same prefix for later index entries.
                    scanToEnd = true;
                }
                const std::int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                const bool beforeEntry = !scanToEnd && indexedPosition >= 0 && packet->pos >= 0 &&
                                         packet->pos < indexedPosition;
                const bool found = packet->stream_index == opened.videoStreamIndex &&
                                   (packet->flags & AV_PKT_FLAG_KEY) != 0 &&
                                   pts != AV_NOPTS_VALUE && !beforeEntry;
                if (found) {
                    times.push_back(av_rescale_q(
                        pts - opened.presentationOrigin, stream.time_base, kMicrosecondsBase));
                    previousPosition = packet->pos;
                }
                av_packet_unref(packet.get());
                if (found && !scanToEnd) {
                    break;
                }
            }
        }
        if (isCanceled(cancelRequested)) {
            return {};
        }
    } else {
        // No index to consult: fall back to walking the packets, which decodes nothing either.
        while (!isCanceled(cancelRequested)) {
            const int readResult = av_read_frame(opened.context.get(), packet.get());
            if (readResult < 0) {
                break;
            }
            const std::int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (packet->stream_index == opened.videoStreamIndex && pts != AV_NOPTS_VALUE &&
                (packet->flags & AV_PKT_FLAG_KEY) != 0) {
                times.push_back(av_rescale_q(
                    pts - opened.presentationOrigin, stream.time_base, kMicrosecondsBase));
            }
            av_packet_unref(packet.get());
        }
    }

    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

application::ClipExportReport ClipExportWriter::perform(const application::ClipExportJob& job,
                                                        const std::atomic_bool& cancelRequested) {
    if (job.sourcePath.empty() || job.outputPath.empty()) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "A clip export needs both a source path and an output path.");
    }
    if (job.plan.endMicroseconds.has_value() &&
        *job.plan.endMicroseconds <= job.plan.startMicroseconds) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "The requested export span ends before it starts.");
    }

    CancelState cancelState{.cancelRequested = &cancelRequested};
    // Asked to stop before any work: FFmpeg's interrupt callback also fails the open and the seek,
    // and those failures would otherwise be reported as a broken source instead of a cancellation.
    if (isCanceled(cancelRequested)) {
        return makeReport(job, application::ClipExportOutcome::kCanceled, 0, {});
    }
    OpenedInput opened = openVideoInput(job.sourcePath, cancelState);
    if (!opened.context) {
        return makeReport(job,
                          isCanceled(cancelRequested) ? application::ClipExportOutcome::kCanceled
                                                      : application::ClipExportOutcome::kFailed,
                          0,
                          std::move(opened.error));
    }
    AVStream* const videoStream = opened.videoStream;

    const std::int64_t startMicroseconds = job.plan.startMicroseconds;
    const std::int64_t startPts =
        opened.presentationOrigin +
        av_rescale_q(startMicroseconds, kMicrosecondsBase, videoStream->time_base);
    const int seekResult = av_seek_frame(
        opened.context.get(), opened.videoStreamIndex, startPts, AVSEEK_FLAG_BACKWARD);
    if (seekResult < 0) {
        // A cancellation that lands during the seek interrupts it; that is not a broken source.
        if (isCanceled(cancelRequested)) {
            return makeReport(job, application::ClipExportOutcome::kCanceled, 0, {});
        }
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "FFmpeg could not seek to the clip start: " + ffmpegError(seekResult));
    }

    // The backward seek lands on the latest keyframe at or before the requested start, and the
    // first packet written is the next keyframe in the stream. Nothing is compared against the
    // start time afterwards: the plan already chose a keyframe instant, and re-deriving it through
    // a timebase rescale would only add a rounding opportunity to skip that very keyframe.
    const std::int64_t endPts =
        job.plan.endMicroseconds.has_value()
            ? opened.presentationOrigin +
                  av_rescale_q(*job.plan.endMicroseconds, kMicrosecondsBase, videoStream->time_base)
            : 0;
    const std::int64_t spanMicroseconds =
        job.plan.endMicroseconds.has_value()
            ? *job.plan.endMicroseconds - startMicroseconds
            : (videoStream->duration > 0
                   ? av_rescale_q(
                         videoStream->duration, videoStream->time_base, kMicrosecondsBase) -
                         startMicroseconds
                   : 0);

    std::error_code pathError;
    if (!job.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(job.outputPath.parent_path(), pathError);
    }
    if (pathError) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "The clip directory could not be created: " + pathError.message());
    }
    const bool replaceExisting = std::filesystem::exists(job.outputPath, pathError);
    if (pathError) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "The clip destination could not be inspected: " + pathError.message());
    }
    auto transaction = platform::AtomicFilePublisher::begin(
        job.outputPath, {.operation = "clip", .ownerId = "export", .revision = job.requestId});
    if (!transaction) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          transaction.error().technicalDetail);
    }
    const auto prepare = transaction.value()->prepareForExternalWrite();
    if (!prepare) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          prepare.error().technicalDetail);
    }
    const std::filesystem::path& workPath = transaction.value()->temporaryPath();

    internal::AvPacketPtr packet{av_packet_alloc()};
    if (!packet) {
        return makeReport(
            job, application::ClipExportOutcome::kFailed, 0, "FFmpeg could not allocate a packet.");
    }

    ClipCopyState copyState;
    copyState.sourceStream = videoStream;
    copyState.presentationOrigin = opened.presentationOrigin;
    copyState.sourceStreamIndex = opened.videoStreamIndex;
    copyState.workPath = &workPath;
    copyState.targetPath = &job.outputPath;
    copyState.cancelState = &cancelState;
    copyState.firstPresentationMicroseconds = startMicroseconds;

    // Packets that present at or after the end bound are held back instead of dropped: a later
    // packet in decode order can still present inside the range and need them as reference frames.
    // Anything still held back when the copy stops is needed by nothing that was written.
    std::vector<internal::AvPacketPtr> heldReferences;

    int reportedPercent = -1;
    bool canceled = false;

    while (true) {
        if (isCanceled(cancelRequested)) {
            canceled = true;
            break;
        }

        const int readResult = av_read_frame(opened.context.get(), packet.get());
        if (readResult < 0) {
            if (readResult != AVERROR_EOF) {
                copyState.failureDetail =
                    "FFmpeg could not read the source: " + ffmpegError(readResult);
            }
            break;
        }
        if (packet->stream_index != opened.videoStreamIndex) {
            av_packet_unref(packet.get());
            continue;
        }

        // Decode order is monotonic in dts and a packet never presents before its own decode
        // timestamp, so dts is the cursor that decides where the copy stops: once it reaches the
        // end bound no packet still to come can present inside the range. Without a bound the
        // export runs to the end of the stream.
        //
        // Which packets are written is decided by presentation time instead, because a reordered
        // stream decodes frames a little after the point where it shows them: filtering on dts
        // would cut the tail short and drag in frames past the out point that the range never asked
        // for.
        const std::int64_t decodeCursor =
            packet->dts != AV_NOPTS_VALUE
                ? packet->dts
                : (packet->pts != AV_NOPTS_VALUE ? packet->pts : startPts);
        const std::int64_t presentationCursor =
            packet->pts != AV_NOPTS_VALUE ? packet->pts : decodeCursor;
        if (job.plan.endMicroseconds.has_value() && decodeCursor >= endPts) {
            av_packet_unref(packet.get());
            break;
        }

        if (job.plan.endMicroseconds.has_value() && presentationCursor >= endPts) {
            internal::AvPacketPtr held{av_packet_clone(packet.get())};
            av_packet_unref(packet.get());
            if (!held) {
                copyState.failureDetail =
                    "FFmpeg could not copy a packet held back for reference frames.";
                break;
            }
            heldReferences.push_back(std::move(held));
            continue;
        }

        // The held-back packets go first: they precede this one in decode order and can be the
        // reference frames it decodes from.
        bool written = true;
        for (internal::AvPacketPtr& held : heldReferences) {
            if (!writeVideoPacket(copyState, held.get())) {
                written = false;
                break;
            }
        }
        heldReferences.clear();
        if (written) {
            written = writeVideoPacket(copyState, packet.get());
        }
        av_packet_unref(packet.get());
        if (!written) {
            break;
        }

        if (job.progress && spanMicroseconds > 0) {
            const double fraction = std::clamp(
                static_cast<double>(
                    av_rescale_q(presentationCursor - opened.presentationOrigin,
                                 videoStream->time_base,
                                 kMicrosecondsBase) -
                    startMicroseconds) /
                    static_cast<double>(spanMicroseconds),
                0.0,
                1.0);
            const int percent = static_cast<int>(fraction * 100.0);
            if (percent > reportedPercent) {
                reportedPercent = percent;
                job.progress(fraction);
            }
        }
    }

    if (!copyState.failureDetail.empty()) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          copyState.packetsWritten,
                          std::move(copyState.failureDetail));
    }
    if (canceled) {
        return makeReport(
            job, application::ClipExportOutcome::kCanceled, copyState.packetsWritten, {});
    }
    if (!copyState.headerWritten) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          0,
                          "The requested span holds no keyframe, so the clip would be empty.");
    }

    const int trailerResult = av_write_trailer(copyState.output.get());
    if (trailerResult < 0) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          copyState.packetsWritten,
                          "FFmpeg could not finalize the clip: " + ffmpegError(trailerResult));
    }
    // Closing flushes AVIO's buffered data and can fail even after a successful trailer.
    // Clear pb here so the context deleter cannot silently close it a second time.
    const int closeResult =
        closeOutputCallback.load(std::memory_order_acquire)(&copyState.output->pb);
    copyState.output.reset();
    if (isCanceled(cancelRequested)) {
        return makeReport(
            job, application::ClipExportOutcome::kCanceled, copyState.packetsWritten, {});
    }
    if (closeResult < 0) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          copyState.packetsWritten,
                          "FFmpeg could not close the clip: " + ffmpegError(closeResult));
    }
    const auto flushed = transaction.value()->flush();
    if (!flushed) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          copyState.packetsWritten,
                          flushed.error().technicalDetail);
    }
    // Publication is the commit point. Cancellation before it leaves the old target intact;
    // cancellation after a successful commit cannot undo a completed export.
    if (isCanceled(cancelRequested)) {
        return makeReport(
            job, application::ClipExportOutcome::kCanceled, copyState.packetsWritten, {});
    }
    const auto published = replaceExisting ? transaction.value()->publishReplacingExisting()
                                           : transaction.value()->publishNew();
    if (!published) {
        return makeReport(job,
                          application::ClipExportOutcome::kFailed,
                          copyState.packetsWritten,
                          published.error().technicalDetail);
    }

    if (job.progress) {
        job.progress(1.0);
    }
    application::ClipExportReport report =
        makeReport(job, application::ClipExportOutcome::kCompleted, copyState.packetsWritten, {});
    report.firstPresentationMicroseconds = copyState.firstPresentationMicroseconds;
    return report;
}

namespace testing {

ScopedClipExportCloseOverride::ScopedClipExportCloseOverride(
    const ClipExportCloseCallback close) noexcept
    : previous_(closeOutputCallback.exchange(close == nullptr ? &avio_closep : close,
                                             std::memory_order_acq_rel)) {}

ScopedClipExportCloseOverride::~ScopedClipExportCloseOverride() {
    closeOutputCallback.store(previous_, std::memory_order_release);
}

} // namespace testing

} // namespace dvs::media
