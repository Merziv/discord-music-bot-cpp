#include "audio_decoder.h"

#include "audio_streamer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <cerrno>
#include <array>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr std::string_view DECODER_STOP_SENTINEL = "__decoder_stopped__";

struct FormatContextDeleter
{
  void operator()(AVFormatContext* ctx) const
  {
    if (ctx != nullptr)
    {
      avformat_close_input(&ctx);
    }
  }
};

struct CodecContextDeleter
{
  void operator()(AVCodecContext* ctx) const
  {
    if (ctx != nullptr)
    {
      avcodec_free_context(&ctx);
    }
  }
};

struct FrameDeleter
{
  void operator()(AVFrame* frame) const
  {
    if (frame != nullptr)
    {
      av_frame_free(&frame);
    }
  }
};

struct PacketDeleter
{
  void operator()(AVPacket* packet) const
  {
    if (packet != nullptr)
    {
      av_packet_free(&packet);
    }
  }
};

struct SwrContextDeleter
{
  void operator()(SwrContext* swr) const
  {
    if (swr != nullptr)
    {
      swr_free(&swr);
    }
  }
};

struct DictionaryHandle
{
  AVDictionary* value{nullptr};

  ~DictionaryHandle()
  {
    av_dict_free(&value);
  }

  DictionaryHandle(const DictionaryHandle&) = delete;
  DictionaryHandle& operator=(const DictionaryHandle&) = delete;

  DictionaryHandle() = default;
};

void ensureNetworkInitialized()
{
  static std::once_flag once;
  std::call_once(once, [] { (void)avformat_network_init(); });
}

[[nodiscard]] std::string ffmpegErrorToString(int errorCode)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(errorCode, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

[[nodiscard]] std::expected<void, std::string> makeFfmpegError(
  std::string_view operation,
  int errorCode)
{
  return std::unexpected(std::format("{} failed: {}", operation, ffmpegErrorToString(errorCode)));
}

[[nodiscard]] int findAudioStreamIndex(const AVFormatContext& formatContext)
{
  for (unsigned int i = 0; i < formatContext.nb_streams; ++i)
  {
    if (formatContext.streams[i] == nullptr || formatContext.streams[i]->codecpar == nullptr)
    {
      continue;
    }

    if (formatContext.streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      return static_cast<int>(i);
    }
  }

  return -1;
}

}  // namespace

std::expected<void, std::string> FFmpegAudioDecoder::decode(
  std::string_view streamUrl,
  const std::stop_token& stopToken,
  SamplesCallback onSamples) const
{
  if (streamUrl.empty())
  {
    return std::unexpected("Decoder input URL is empty");
  }
  if (!onSamples)
  {
    return std::unexpected("Decoder callback is not set");
  }
  if (stopToken.stop_requested())
  {
    return {};
  }

  ensureNetworkInitialized();

  const std::string streamUrlStorage(streamUrl);
  AVFormatContext* formatRaw = nullptr;

  DictionaryHandle openOptions;
  av_dict_set(&openOptions.value, "reconnect", "1", 0);
  av_dict_set(&openOptions.value, "reconnect_streamed", "1", 0);
  av_dict_set(&openOptions.value, "reconnect_delay_max", "5", 0);
  av_dict_set(&openOptions.value, "rw_timeout", "15000000", 0);

  int rc = avformat_open_input(
    &formatRaw,
    streamUrlStorage.c_str(),
    nullptr,
    &openOptions.value);
  if (rc < 0)
  {
    return makeFfmpegError("avformat_open_input", rc);
  }

  std::unique_ptr<AVFormatContext, FormatContextDeleter> formatContext(formatRaw);

  rc = avformat_find_stream_info(formatContext.get(), nullptr);
  if (rc < 0)
  {
    return makeFfmpegError("avformat_find_stream_info", rc);
  }

  const int audioStreamIndex = findAudioStreamIndex(*formatContext);
  if (audioStreamIndex < 0)
  {
    return std::unexpected("No audio stream found");
  }

  AVStream* audioStream = formatContext->streams[audioStreamIndex];
  if (audioStream == nullptr || audioStream->codecpar == nullptr)
  {
    return std::unexpected("Audio stream metadata is incomplete");
  }

  const AVCodec* codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
  if (codec == nullptr)
  {
    return std::unexpected("No decoder found for audio stream codec");
  }

  std::unique_ptr<AVCodecContext, CodecContextDeleter> codecContext(
    avcodec_alloc_context3(codec));
  if (!codecContext)
  {
    return std::unexpected("Failed to allocate AVCodecContext");
  }

  rc = avcodec_parameters_to_context(codecContext.get(), audioStream->codecpar);
  if (rc < 0)
  {
    return makeFfmpegError("avcodec_parameters_to_context", rc);
  }

  codecContext->pkt_timebase = audioStream->time_base;
  codecContext->thread_count = 1;

  rc = avcodec_open2(codecContext.get(), codec, nullptr);
  if (rc < 0)
  {
    return makeFfmpegError("avcodec_open2", rc);
  }

  if (codecContext->sample_rate <= 0)
  {
    return std::unexpected("Invalid decoder sample rate");
  }

  AVChannelLayout outChannelLayout{};
  av_channel_layout_default(&outChannelLayout, audio::CHANNELS);

  AVChannelLayout fallbackInLayout{};
  const AVChannelLayout* inLayout = &codecContext->ch_layout;
  bool usesFallbackInLayout = false;
  if (inLayout->nb_channels <= 0)
  {
    av_channel_layout_default(&fallbackInLayout, audio::CHANNELS);
    inLayout = &fallbackInLayout;
    usesFallbackInLayout = true;
  }

  SwrContext* swrRaw = nullptr;
  rc = swr_alloc_set_opts2(
    &swrRaw,
    &outChannelLayout,
    AV_SAMPLE_FMT_S16,
    audio::SAMPLE_RATE,
    inLayout,
    codecContext->sample_fmt,
    codecContext->sample_rate,
    0,
    nullptr);

  av_channel_layout_uninit(&outChannelLayout);
  if (usesFallbackInLayout)
  {
    av_channel_layout_uninit(&fallbackInLayout);
  }

  if (rc < 0 || swrRaw == nullptr)
  {
    return makeFfmpegError("swr_alloc_set_opts2", rc);
  }

  std::unique_ptr<SwrContext, SwrContextDeleter> swr(swrRaw);

  rc = swr_init(swr.get());
  if (rc < 0)
  {
    return makeFfmpegError("swr_init", rc);
  }

  std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  if (!packet || !frame)
  {
    return std::unexpected("Failed to allocate AVPacket/AVFrame");
  }

  std::vector<int16_t> pcmBuffer;

  auto drainFrames = [&]() -> std::expected<void, std::string> {
    while (!stopToken.stop_requested())
    {
      rc = avcodec_receive_frame(codecContext.get(), frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
      {
        return {};
      }
      if (rc < 0)
      {
        return makeFfmpegError("avcodec_receive_frame", rc);
      }

      const int64_t delayedSamples = swr_get_delay(swr.get(), codecContext->sample_rate);
      const int outSamples = static_cast<int>(av_rescale_rnd(
        delayedSamples + frame->nb_samples,
        audio::SAMPLE_RATE,
        codecContext->sample_rate,
        AV_ROUND_UP));

      if (outSamples > 0)
      {
        const size_t totalSamples =
          static_cast<size_t>(outSamples) * static_cast<size_t>(audio::CHANNELS);
        pcmBuffer.resize(totalSamples);

        const bool planarInput = av_sample_fmt_is_planar(codecContext->sample_fmt) != 0;
        const size_t inputPlaneCount =
          planarInput && frame->ch_layout.nb_channels > 0
            ? static_cast<size_t>(frame->ch_layout.nb_channels)
            : size_t{1};
        std::vector<const uint8_t*> inputPlanes(inputPlaneCount);
        for (size_t plane = 0; plane < inputPlaneCount; ++plane)
        {
          inputPlanes[plane] = frame->extended_data[plane];
        }

        // swr_convert expects planar pointers. We provide one interleaved output plane for S16.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* outputBytes = reinterpret_cast<uint8_t*>(pcmBuffer.data());
        uint8_t* outputPlanes[] = {outputBytes};
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

        const int converted = swr_convert(
          swr.get(),
          outputPlanes,
          outSamples,
          inputPlanes.data(),
          frame->nb_samples);

        if (converted < 0)
        {
          av_frame_unref(frame.get());
          return makeFfmpegError("swr_convert", converted);
        }

        if (converted > 0)
        {
          const size_t producedSamples =
            static_cast<size_t>(converted) * static_cast<size_t>(audio::CHANNELS);
          if (!onSamples(std::span<const int16_t>(pcmBuffer.data(), producedSamples)))
          {
            av_frame_unref(frame.get());
            return std::unexpected(std::string(DECODER_STOP_SENTINEL));
          }
        }
      }

      av_frame_unref(frame.get());
    }

    return std::unexpected(std::string(DECODER_STOP_SENTINEL));
  };

  while (!stopToken.stop_requested())
  {
    rc = av_read_frame(formatContext.get(), packet.get());
    if (rc == AVERROR_EOF)
    {
      break;
    }
    if (rc == AVERROR(EAGAIN))
    {
      continue;
    }
    if (rc < 0)
    {
      return makeFfmpegError("av_read_frame", rc);
    }

    if (packet->stream_index != audioStreamIndex)
    {
      av_packet_unref(packet.get());
      continue;
    }

    while (true)
    {
      rc = avcodec_send_packet(codecContext.get(), packet.get());
      if (rc == AVERROR(EAGAIN))
      {
        auto drained = drainFrames();
        if (!drained)
        {
          if (drained.error() == DECODER_STOP_SENTINEL)
          {
            av_packet_unref(packet.get());
            return {};
          }
          return std::unexpected(drained.error());
        }
        continue;
      }
      break;
    }

    av_packet_unref(packet.get());

    if (rc < 0)
    {
      return makeFfmpegError("avcodec_send_packet", rc);
    }

    auto drained = drainFrames();
    if (!drained)
    {
      if (drained.error() == DECODER_STOP_SENTINEL)
      {
        return {};
      }
      return std::unexpected(drained.error());
    }
  }

  if (!stopToken.stop_requested())
  {
    rc = avcodec_send_packet(codecContext.get(), nullptr);
    if (rc >= 0 || rc == AVERROR_EOF)
    {
      auto drained = drainFrames();
      if (!drained && drained.error() != DECODER_STOP_SENTINEL)
      {
        return std::unexpected(drained.error());
      }
    }
    else
    {
      return makeFfmpegError("avcodec_send_packet(flush)", rc);
    }
  }

  return {};
}