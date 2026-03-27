#include "audio_streamer.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace audio {

AudioStreamer::AudioStreamer(
  std::string streamUrl,
  std::string title,
  PlaybackController controller)
  : _streamUrl(std::move(streamUrl))
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

  int pid = _ffmpegPid.load(std::memory_order_acquire);
  if (pid > 0)
  {
    ::kill(pid, SIGTERM);
  }

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
  const std::string sampleRateStr = std::to_string(SAMPLE_RATE);
  const std::string channelsStr = std::to_string(CHANNELS);

  int pipefd[2];
  if (pipe(pipefd) != 0)
  {
    logging::error("Failed to create pipe for ffmpeg");
    _producerDone.store(true, std::memory_order_release);
    return;
  }

  pid_t pid = fork();
  if (pid < 0)
  {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    logging::error("Failed to fork for ffmpeg");
    _producerDone.store(true, std::memory_order_release);
    return;
  }

  if (pid == 0)
  {
    ::close(pipefd[0]);

    if (dup2(pipefd[1], STDOUT_FILENO) < 0)
    {
      _exit(127);
    }
    ::close(pipefd[1]);

    int errfd = ::open(ffmpegErrLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (errfd >= 0)
    {
      dup2(errfd, STDERR_FILENO);
      ::close(errfd);
    }

    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0)
    {
      dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    execlp(
      "ffmpeg",
      "ffmpeg",
      "-hide_banner",
      "-loglevel",
      "warning",
      "-nostdin",
      "-reconnect",
      "1",
      "-reconnect_streamed",
      "1",
      "-reconnect_delay_max",
      "5",
      "-fflags",
      "+nobuffer+discardcorrupt",
      "-flags",
      "low_delay",
      "-probesize",
      "1M",
      "-analyzeduration",
      "0",
      "-i",
      _streamUrl.c_str(),
      "-ar",
      sampleRateStr.c_str(),
      "-ac",
      channelsStr.c_str(),
      "-f",
      "s16le",
      "-vn",
      "pipe:1",
      static_cast<char*>(nullptr));
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)

    _exit(127);
  }

  ::close(pipefd[1]);
  int fd = pipefd[0];

  fcntl(fd, F_SETPIPE_SZ, 1024 * 1024);
  _ffmpegPid.store(static_cast<int>(pid), std::memory_order_release);

  logging::debug(
    "Launched ffmpeg pid={} for '{}', stderr log: {}",
    static_cast<int>(pid),
    _title,
    ffmpegErrLog);

  AudioFrame frame{};
  size_t frameOffset = 0;
  std::array<char, 131072> readBuf{};
  constexpr int POLL_TIMEOUT_MS = 200;

  while (!stopToken.stop_requested() && !_shouldStop.load(std::memory_order_acquire)
         && !_controller.shouldStop())
  {
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    int pollResult = poll(&pfd, 1, POLL_TIMEOUT_MS);

    if (pollResult == 0)
    {
      continue;
    }
    if (pollResult < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      logging::warn("Poll error on ffmpeg pipe: {}", strerror(errno));
      break;
    }

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
          if (++spinCount > 10)
          {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

  ::close(fd);
  _ffmpegPid.store(-1, std::memory_order_release);

  ::kill(pid, SIGTERM);

  int status = 0;
  for (int i = 0; i < 20; ++i)
  {
    if (waitpid(pid, &status, WNOHANG) != 0)
    {
      pid = -1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (pid > 0)
  {
    ::kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  _producerDone.store(true, std::memory_order_release);
  logging::info("Audio producer finished");
}

void AudioStreamer::consumerLoop()
{
  using Clock = std::chrono::steady_clock;
  constexpr auto frameDuration = std::chrono::milliseconds(FRAME_DURATION_MS);

  constexpr size_t PREBUFFER_FRAMES = 50;
  constexpr auto PREBUFFER_TIMEOUT = std::chrono::seconds(15);
  auto prebufferStart = Clock::now();

  while (_ringBuffer.size() < PREBUFFER_FRAMES && !_producerDone.load(std::memory_order_acquire)
         && !_shouldStop.load(std::memory_order_acquire) && !_controller.shouldStop()
         && (Clock::now() - prebufferStart) < PREBUFFER_TIMEOUT)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (_ringBuffer.empty())
  {
    logging::warn("No audio data available after pre-buffer phase");
    logging::info("Audio consumer finished");
    return;
  }

  logging::info("Pre-buffer complete, starting playback. Buffer size: {}", _ringBuffer.size());
  {
    constexpr int MAX_READY_POLLS = 50;
    for (int i = 0; i < MAX_READY_POLLS; ++i)
    {
      if (_controller.isReady())
      {
        logging::info("Voice client confirmed ready (poll {})", i);
        break;
      }
      if (_shouldStop.load(std::memory_order_acquire) || _controller.shouldStop())
      {
        logging::warn("Stopped while waiting for voice readiness");
        logging::info("Audio consumer finished");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!_controller.isReady())
    {
      logging::error("Voice client not ready after 5s, aborting playback");
      logging::info("Audio consumer finished");
      return;
    }
  }

  // Set overlap audio mode so DPP sends each opus packet immediately via UDP,
  // bypassing DPP's internal send thread which can get stuck after reconnections.
  _controller.configureVoiceClient();

  std::vector<unsigned char> opusBuf(4000);
  auto nextFrameTime = Clock::now();
  bool stoppedEarly = false;
  size_t sendCount = 0;
  _playedAudio.store(false, std::memory_order_release);

  while (!_shouldStop.load(std::memory_order_acquire) && !_controller.shouldStop())
  {
    if (_controller.isPaused())
    {
      if (!_controller.waitWhilePaused())
      {
        stoppedEarly = true;
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
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
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

    if (!_controller.trySendAudioOpus(opusBuf.data(), static_cast<size_t>(bytes)))
    {
      logging::warn("Voice client disconnected, stopping playback");
      stoppedEarly = true;
      break;
    }

    _playedAudio.store(true, std::memory_order_release);
    ++sendCount;

    if (sendCount % 500 == 1)
    {
      logging::debug(
        "Audio watchdog: sends={}, ring={}",
        sendCount,
        _ringBuffer.size());
    }

    nextFrameTime += frameDuration;
    auto now = Clock::now();

    if (nextFrameTime > now)
    {
      std::this_thread::sleep_until(nextFrameTime);
    }
    else if (now - nextFrameTime > std::chrono::milliseconds(100))
    {
      logging::warn("Audio timing reset due to lag (drift={}ms)",
        std::chrono::duration_cast<std::chrono::milliseconds>(now - nextFrameTime).count());
      nextFrameTime = now;
    }
  }

  if (stoppedEarly || _shouldStop.load(std::memory_order_acquire) || _controller.shouldStop())
  {
    _controller.tryStopAudio();
    logging::info("Audio stopped early, cleared DPP buffer");
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

}  // namespace audio
