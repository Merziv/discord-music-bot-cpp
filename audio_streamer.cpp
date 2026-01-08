#include "audio_streamer.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace audio {

AudioStreamer::AudioStreamer(
  dpp::discord_voice_client* voiceClient,
  std::string streamUrl,
  std::string title,
  PlaybackController controller)
  : _voiceClient(voiceClient)
  , _streamUrl(std::move(streamUrl))
  , _title(std::move(title))
  , _controller(std::move(controller))
{
  initEncoder();
}

AudioStreamer::~AudioStreamer()
{
  stop();
}

void AudioStreamer::start()
{
  _producerThread = std::jthread([this](const std::stop_token& st) { producerLoop(st); });
  consumerLoop();
}

void AudioStreamer::stop()
{
  _shouldStop.store(true, std::memory_order_release);
  if (_producerThread.joinable())
  {
    _producerThread.request_stop();
    _producerThread.join();
  }
}

void AudioStreamer::initEncoder()
{
  int err = 0;
  _encoder.reset(opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err));
  if (!_encoder || err != OPUS_OK)
  {
    throw std::runtime_error(std::format("Opus encoder init failed: {}", opus_strerror(err)));
  }

  opus_encoder_ctl(_encoder.get(), OPUS_SET_BITRATE(OPUS_BITRATE));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_VBR(1));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_VBR_CONSTRAINT(1));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_COMPLEXITY(10));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_INBAND_FEC(0));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_DTX(0));
  opus_encoder_ctl(_encoder.get(), OPUS_SET_PACKET_LOSS_PERC(0));
}

void AudioStreamer::producerLoop(const std::stop_token& stopToken)
{
  const std::string ffmpegErrLog = makeTempLogPath("ffmpeg_audio");
  const std::string ffmpegCmd = std::format(
    "ffmpeg -hide_banner -loglevel warning -nostdin "
    "-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
    "-fflags +nobuffer+discardcorrupt -flags low_delay "
    "-probesize 32M -analyzeduration 0 "
    "-i {} "
    "-ar {} -ac {} "
    "-f s16le -vn pipe:1 2>{}",
    quote(_streamUrl),
    SAMPLE_RATE,
    CHANNELS,
    quote(ffmpegErrLog));

  logging::debug("Launching ffmpeg: {}", ffmpegCmd);

  FILE* pipe = popen(ffmpegCmd.c_str(), "r");
  if (pipe == nullptr)
  {
    logging::error("Failed to start ffmpeg");
    _producerDone.store(true, std::memory_order_release);
    return;
  }

  int fd = fileno(pipe);
  fcntl(fd, F_SETPIPE_SZ, 1024 * 1024);

  AudioFrame frame{};
  size_t frameOffset = 0;
  std::array<char, 65536> readBuf{};

  while (!stopToken.stop_requested() && !_shouldStop.load(std::memory_order_acquire)
         && !_controller.shouldStop())
  {
    ssize_t bytesRead = ::read(fd, readBuf.data(), readBuf.size());

    if (bytesRead <= 0)
    {
      if (bytesRead == 0)
      {
        logging::info("FFmpeg stream ended");
      }
      else
      {
        logging::warn("Read error from ffmpeg pipe");
      }
      break;
    }

    size_t pos = 0;
    while (pos < static_cast<size_t>(bytesRead))
    {
      const size_t frameRemaining = FRAME_BYTES - frameOffset;
      const size_t available = static_cast<size_t>(bytesRead) - pos;
      const size_t toCopy = std::min(frameRemaining, available);

      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      auto* frameBytes = reinterpret_cast<char*>(frame.data());
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      std::memcpy(frameBytes + frameOffset, readBuf.data() + pos, toCopy);
      frameOffset += toCopy;
      pos += toCopy;

      if (frameOffset >= static_cast<size_t>(FRAME_BYTES))
      {
        int spinCount = 0;
        while (!_ringBuffer.push(frame))
        {
          if (
            stopToken.stop_requested() || _shouldStop.load(std::memory_order_acquire)
            || _controller.shouldStop())
          {
            break;
          }
          if (++spinCount > 1000)
          {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            spinCount = 0;
          }
          else
          {
            std::this_thread::yield();
          }
        }
        frameOffset = 0;
      }
    }
  }

  pclose(pipe);
  _producerDone.store(true, std::memory_order_release);
  logging::info("Audio producer finished");
}

void AudioStreamer::consumerLoop()
{
  _voiceClient->set_timescale(1'000'000);
  _voiceClient->set_send_audio_type(dpp::discord_voice_client::satype_overlap_audio);

  std::vector<unsigned char> opusBuf(10000);

  using Clock = std::chrono::steady_clock;
  auto nextFrameTime = Clock::now();
  constexpr auto frameDuration = std::chrono::milliseconds(FRAME_DURATION_MS);

  constexpr size_t PREBUFFER_FRAMES = 50;
  while (_ringBuffer.size() < PREBUFFER_FRAMES && !_producerDone.load(std::memory_order_acquire)
         && !_shouldStop.load(std::memory_order_acquire) && !_controller.shouldStop())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  logging::info("Pre-buffer complete, starting playback. Buffer size: {}", _ringBuffer.size());

  while (!_shouldStop.load(std::memory_order_acquire) && !_controller.shouldStop())
  {
    if (_controller.isPaused())
    {
      if (!_controller.waitWhilePaused())
      {
        break;
      }
      nextFrameTime = Clock::now();
    }

    auto frameOpt = _ringBuffer.pop();

    if (!frameOpt)
    {
      if (_producerDone.load(std::memory_order_acquire))
      {
        logging::info("Buffer drained, playback complete");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    const auto& frame = *frameOpt;

    int bytes = opus_encode(
      _encoder.get(),
      frame.data(),
      SAMPLES_PER_CHANNEL,
      opusBuf.data(),
      static_cast<opus_int32>(opusBuf.size()));

    if (bytes < 0)
    {
      logging::warn("Opus encode failed: {}", opus_strerror(bytes));
      continue;
    }

    _voiceClient->send_audio_opus(opusBuf.data(), static_cast<size_t>(bytes));

    nextFrameTime += frameDuration;
    auto now = Clock::now();

    if (nextFrameTime > now)
    {
      std::this_thread::sleep_until(nextFrameTime);
    }
    else if (now - nextFrameTime > std::chrono::milliseconds(100))
    {
      nextFrameTime = now;
      logging::warn("Audio timing reset due to lag");
    }
  }

  logging::info("Audio consumer finished");
}

std::string AudioStreamer::makeTempLogPath(const char* prefix)
{
  std::string path = std::format("/tmp/{}_XXXXXX.log", prefix);
  std::vector<char> tmpl(path.begin(), path.end());
  tmpl.push_back('\0');

  if (int fd = mkstemps(tmpl.data(), 4); fd >= 0)
  {
    close(fd);
  }
  return {tmpl.data()};
}

std::string AudioStreamer::quote(std::string_view str)
{
  std::string out;
  out.reserve(str.size() + 2);
  out.push_back('"');
  for (char c : str)
  {
    if (c == '"')
    {
      out += "\\\"";
    }
    else
    {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace audio
