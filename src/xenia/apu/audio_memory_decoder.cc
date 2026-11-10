/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/apu/audio_memory_decoder.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include "xenia/apu/media_memory_io.h"
extern "C" {
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavformat/avformat.h"
#include "third_party/FFmpeg/libavutil/samplefmt.h"
}
namespace xe::apu {
bool PcmLayoutMatchesChannels(PcmChannelLayout layout, uint32_t channels) {
  switch (layout) {
    case PcmChannelLayout::kMono:
      return channels == 1;
    case PcmChannelLayout::kStereo:
      return channels == 2;
    case PcmChannelLayout::kSurround51Back:
    case PcmChannelLayout::kSurround51Side:
      return channels == 6;
    default:
      return false;
  }
}
bool PcmFormatTracker::Accept(const AudioFrameFormat& format, PcmAudio& pcm) {
  if (format.frequency < 8000 || format.frequency > 192000) {
    return false;
  }
  PcmChannelLayout layout = PcmChannelLayout::kUnknown;
  // Native WAVE/FFmpeg channel masks: FC; FL/FR; FL/FR/FC/LFE/BL/BR;
  // FL/FR/FC/LFE/SL/SR. No top/wide/unknown speaker can enter a fixed sink.
  if (format.order == AudioChannelOrder::kNative) {
    if (format.native_mask == 0x4 && format.channels == 1) {
      layout = PcmChannelLayout::kMono;
    }
    if (format.native_mask == 0x3 && format.channels == 2) {
      layout = PcmChannelLayout::kStereo;
    }
    if (format.native_mask == 0x3F && format.channels == 6) {
      layout = PcmChannelLayout::kSurround51Back;
    }
    if (format.native_mask == 0x60F && format.channels == 6) {
      layout = PcmChannelLayout::kSurround51Side;
    }
  } else if (format.order == AudioChannelOrder::kUnspecified &&
             !format.native_mask) {
    if (format.channels == 1) {
      layout = PcmChannelLayout::kMono;
    }
    if (format.channels == 2) {
      layout = PcmChannelLayout::kStereo;
    }
  }
  if (!PcmLayoutMatchesChannels(layout, format.channels)) {
    return false;
  }
  if (source_) {
    return *source_ == format && pcm.frequency == format.frequency &&
           pcm.channels == format.channels && pcm.layout == layout;
  }
  if (pcm.frequency || pcm.channels ||
      pcm.layout != PcmChannelLayout::kUnknown || !pcm.samples.empty()) {
    return false;
  }
  source_ = format;
  pcm.frequency = format.frequency;
  pcm.channels = format.channels;
  pcm.layout = layout;
  return true;
}
namespace {
struct Work {
  const std::atomic<bool>* cancelled;
  std::chrono::steady_clock::time_point deadline;
  bool Interrupted() const {
    return (cancelled && cancelled->load(std::memory_order_acquire)) ||
           std::chrono::steady_clock::now() >= deadline;
  }
};
struct Context {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  ~Context() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
};
int Append(const AVFrame& frame, PcmAudio& pcm, PcmFormatTracker& formats,
           size_t maximum) {
  const int channels = frame.ch_layout.nb_channels;
  if ((frame.flags & AV_FRAME_FLAG_CORRUPT) || frame.nb_samples < 0 ||
      channels < 1 || channels > 6 || frame.sample_rate < 8000 ||
      frame.sample_rate > 192000 || !frame.extended_data) {
    return AVERROR_INVALIDDATA;
  }
  AudioFrameFormat format;
  format.frequency = uint32_t(frame.sample_rate);
  format.channels = uint32_t(channels);
  switch (frame.ch_layout.order) {
    case AV_CHANNEL_ORDER_UNSPEC:
      format.order = AudioChannelOrder::kUnspecified;
      break;
    case AV_CHANNEL_ORDER_NATIVE:
      format.order = AudioChannelOrder::kNative;
      format.native_mask = frame.ch_layout.u.mask;
      break;
    case AV_CHANNEL_ORDER_CUSTOM:
      format.order = AudioChannelOrder::kCustom;
      break;
    case AV_CHANNEL_ORDER_AMBISONIC:
      format.order = AudioChannelOrder::kAmbisonic;
      break;
    default:
      return AVERROR(ENOSYS);
  }
  if (!formats.Accept(format, pcm)) {
    return AVERROR(ENOSYS);
  }
  if (size_t(frame.nb_samples) >
      (maximum - pcm.samples.size()) / pcm.channels) {
    return AVERROR(EFBIG);
  }
  const auto sample_format = static_cast<AVSampleFormat>(frame.format);
  const auto packed = av_get_packed_sample_fmt(sample_format);
  const bool planar = av_sample_fmt_is_planar(sample_format);
  const int width = av_get_bytes_per_sample(sample_format);
  if (width <= 0) {
    return AVERROR(ENOSYS);
  }
  for (int ch = 0; ch < channels; ++ch) {
    if (!frame.extended_data[planar ? ch : 0]) {
      return AVERROR_INVALIDDATA;
    }
  }
  for (int i = 0; i < frame.nb_samples; ++i) {
    for (int ch = 0; ch < channels; ++ch) {
      const uint8_t* data = frame.extended_data[planar ? ch : 0] +
                            size_t(planar ? i : i * channels + ch) * width;
      double value;
      switch (packed) {
        case AV_SAMPLE_FMT_FLT: {
          float v;
          std::memcpy(&v, data, 4);
          value = v;
          break;
        }
        case AV_SAMPLE_FMT_DBL: {
          double v;
          std::memcpy(&v, data, 8);
          value = v;
          break;
        }
        case AV_SAMPLE_FMT_S16: {
          int16_t v;
          std::memcpy(&v, data, 2);
          value = v / 32768.0;
          break;
        }
        case AV_SAMPLE_FMT_S32: {
          int32_t v;
          std::memcpy(&v, data, 4);
          value = v / 2147483648.0;
          break;
        }
        case AV_SAMPLE_FMT_S64: {
          int64_t v;
          std::memcpy(&v, data, 8);
          value = double(v) / 9223372036854775808.0;
          break;
        }
        case AV_SAMPLE_FMT_U8:
          value = (int(*data) - 128) / 128.0;
          break;
        default:
          return AVERROR(ENOSYS);
      }
      if (!std::isfinite(value)) {
        return AVERROR_INVALIDDATA;
      }
      pcm.samples.push_back(float(value));
    }
  }
  return 0;
}
AudioDecodeResult Decode(std::vector<uint8_t> bytes, AudioDecodeLimits limits,
                         const std::atomic<bool>* cancelled) {
  AudioDecodeResult result;
  if (bytes.empty() || !limits.maximum_input_bytes ||
      !limits.maximum_sample_values ||
      limits.maximum_sample_values > 64u * 1024u * 1024u ||
      limits.maximum_input_bytes > 256u * 1024u * 1024u ||
      !limits.maximum_packets || limits.maximum_packets > 1048576 ||
      limits.maximum_work_time <= std::chrono::milliseconds::zero() ||
      limits.maximum_work_time > std::chrono::minutes(1)) {
    return result;
  }
  if (bytes.size() > limits.maximum_input_bytes) {
    result.status = AudioDecodeStatus::kLimitExceeded;
    return result;
  }
  Work work{cancelled,
            std::chrono::steady_clock::now() + limits.maximum_work_time};
  auto io = MediaMemoryIo::Create(std::move(bytes), limits.maximum_input_bytes);
  if (!io) {
    result.status = AudioDecodeStatus::kNoMemory;
    return result;
  }
  Context context;
  context.format = avformat_alloc_context();
  if (!context.format) {
    result.status = AudioDecodeStatus::kNoMemory;
    return result;
  }
  context.format->pb = io->context();
  context.format->flags |= AVFMT_FLAG_CUSTOM_IO;
  context.format->probesize = 65536;
  context.format->max_analyze_duration = 1000000;
  context.format->max_streams = 8;
  context.format->max_probe_packets = 64;
  context.format->interrupt_callback = {
      [](void* p) { return static_cast<Work*>(p)->Interrupted() ? 1 : 0; },
      &work};
  context.format->io_open = [](AVFormatContext*, AVIOContext**, const char*,
                               int, AVDictionary**) { return AVERROR(EACCES); };
  AVDictionary* options = nullptr;
  if (av_dict_set(&options, "format_whitelist", "wav,asf,mp3", 0) < 0 ||
      av_dict_set(&options, "protocol_whitelist", "", 0) < 0) {
    av_dict_free(&options);
    result.status = AudioDecodeStatus::kNoMemory;
    return result;
  }
  int error = avformat_open_input(&context.format, nullptr, nullptr, &options);
  av_dict_free(&options);
  auto fail = [&](int code,
                  AudioDecodeStatus status = AudioDecodeStatus::kCorrupt) {
    AudioDecodeResult failed;
    failed.codec_error = code;
    if (code == AVERROR(EFBIG)) {
      status = AudioDecodeStatus::kLimitExceeded;
    }
    if (code == AVERROR(ENOSYS) || code == AVERROR_DECODER_NOT_FOUND) {
      status = AudioDecodeStatus::kUnsupported;
    }
    if (code == AVERROR(ENOMEM)) {
      status = AudioDecodeStatus::kNoMemory;
    }
    failed.status = work.Interrupted()
                        ? (cancelled && cancelled->load()
                               ? AudioDecodeStatus::kCancelled
                               : AudioDecodeStatus::kLimitExceeded)
                        : status;
    return failed;
  };
  if (error < 0) {
    return fail(error);
  }
  // Do not let find_stream_info create/decode with an unbounded private codec
  // context. These demuxers supply codec identity/extra data in read_header;
  // MP3's sample rate/layout are learned from the first bounded decoded frame.
  if (context.format->nb_streams > 8) {
    return fail(AVERROR(EFBIG));
  }
  const AVCodec* codec = nullptr;
  int stream = -1;
  for (unsigned i = 0; i < context.format->nb_streams; ++i) {
    const auto* parameters = context.format->streams[i]->codecpar;
    if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) {
      continue;
    }
    // A complete finite clip has one audio stream. Choosing a movie's track
    // belongs to a later explicit media selection API, never a hidden guess.
    if (stream != -1) {
      return fail(AVERROR(ENOSYS), AudioDecodeStatus::kUnsupported);
    }
    stream = int(i);
    codec = avcodec_find_decoder(parameters->codec_id);
  }
  if (stream < 0 || !codec) {
    return fail(AVERROR_DECODER_NOT_FOUND, AudioDecodeStatus::kUnsupported);
  }
  context.codec = avcodec_alloc_context3(codec);
  if (!context.codec) {
    return fail(AVERROR(ENOMEM), AudioDecodeStatus::kNoMemory);
  }
  if ((error = avcodec_parameters_to_context(
           context.codec, context.format->streams[stream]->codecpar)) < 0) {
    return fail(error);
  }
  context.codec->thread_count = 1;
  context.codec->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
  context.codec->max_samples =
      int64_t(std::min(limits.maximum_sample_values, size_t(1048576)));
  if (context.codec->ch_layout.nb_channels > 6 ||
      context.codec->sample_rate > 192000) {
    return fail(AVERROR(ENOSYS), AudioDecodeStatus::kUnsupported);
  }
  if ((error = avcodec_open2(context.codec, codec, nullptr)) < 0) {
    return fail(error);
  }
  context.packet = av_packet_alloc();
  context.frame = av_frame_alloc();
  if (!context.packet || !context.frame) {
    return fail(AVERROR(ENOMEM), AudioDecodeStatus::kNoMemory);
  }
  PcmFormatTracker formats;
  auto receive = [&]() {
    for (;;) {
      if (work.Interrupted()) {
        return AVERROR_EXIT;
      }
      const int ret = avcodec_receive_frame(context.codec, context.frame);
      if (ret < 0) {
        return ret;
      }
      const int appended = Append(*context.frame, result.pcm, formats,
                                  limits.maximum_sample_values);
      if (appended < 0) {
        return appended;
      }
      av_frame_unref(context.frame);
    }
  };
  uint32_t packets = 0;
  while ((error = av_read_frame(context.format, context.packet)) >= 0) {
    if (work.Interrupted()) {
      return fail(AVERROR_EXIT);
    }
    if (++packets > limits.maximum_packets || context.packet->size < 0 ||
        context.packet->size > 4 * 1024 * 1024) {
      return fail(AVERROR(EFBIG), AudioDecodeStatus::kLimitExceeded);
    }
    if (context.packet->flags & AV_PKT_FLAG_CORRUPT) {
      return fail(AVERROR_INVALIDDATA);
    }
    if (context.packet->stream_index == stream) {
      int sent = avcodec_send_packet(context.codec, context.packet);
      if (sent == AVERROR(EAGAIN)) {
        const int ret = receive();
        if (ret != AVERROR(EAGAIN)) {
          return fail(ret);
        }
        sent = avcodec_send_packet(context.codec, context.packet);
      }
      if (sent < 0) {
        return fail(sent);
      }
      const int ret = receive();
      if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return fail(ret, ret == AVERROR(EFBIG)
                             ? AudioDecodeStatus::kLimitExceeded
                             : AudioDecodeStatus::kCorrupt);
      }
    }
    av_packet_unref(context.packet);
  }
  if (error != AVERROR_EOF) {
    return fail(error);
  }
  error = avcodec_send_packet(context.codec, nullptr);
  if (error < 0 && error != AVERROR_EOF) {
    return fail(error);
  }
  error = receive();
  if (error != AVERROR_EOF || result.pcm.samples.empty()) {
    return fail(error);
  }
  result.status = AudioDecodeStatus::kSuccess;
  return result;
}
}  // namespace
AudioDecodeResult DecodeAudioMemory(std::vector<uint8_t> bytes,
                                    AudioDecodeLimits limits,
                                    const std::atomic<bool>* cancelled) {
  try {
    return Decode(std::move(bytes), limits, cancelled);
  } catch (const std::bad_alloc&) {
    AudioDecodeResult result;
    result.status = AudioDecodeStatus::kNoMemory;
    return result;
  }
}
}  // namespace xe::apu
