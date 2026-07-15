#include "audio_streamer.h"

#include "audio_decoder.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace audio {

AudioStreamer::AudioStreamer(
  std::string streamUrl,
  std::string title,
  bool isLive,
  StreamRefreshCallback refreshStreamUrl,
  PlaybackController controller)
  : _streamUrl(std::move(streamUrl))
  , _title(std::move(title))
  , _isLive(isLive)
  , _refreshStreamUrl(std::move(refreshStreamUrl))
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
  _producerDone.store(false, std::memory_order_release);
  _shouldStop.store(false, std::memory_order_release);
  _playedAudio.store(false, std::memory_order_release);
  _ringBuffer.clear();

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
  auto shouldAbort = [&] {
    return stopToken.stop_requested() || _shouldStop.load(std::memory_order_acquire)
           || _controller.shouldStop();
  };

  auto pushFrame = [&](const AudioFrame& frame) {
    int spinCount = 0;
    while (!_ringBuffer.push(frame))
    {
      if (shouldAbort())
      {
        return false;
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
    return true;
  };

  FFmpegAudioDecoder decoder;
  AudioFrame frame{};
  size_t frameOffset = 0;
  std::string currentStreamUrl = _streamUrl;

  auto onSamples = [&](std::span<const int16_t> samples) -> bool {
    size_t pos = 0;

    while (pos < samples.size())
    {
      if (shouldAbort())
      {
        return false;
      }

      const size_t remainingInFrame = static_cast<size_t>(TOTAL_SAMPLES) - frameOffset;
      const size_t remainingInChunk = samples.size() - pos;
      const size_t toCopy = std::min(remainingInFrame, remainingInChunk);

      std::copy_n(samples.data() + static_cast<std::ptrdiff_t>(pos),
                  static_cast<std::ptrdiff_t>(toCopy),
                  frame.data() + static_cast<std::ptrdiff_t>(frameOffset));

      frameOffset += toCopy;
      pos += toCopy;

      if (frameOffset >= static_cast<size_t>(TOTAL_SAMPLES))
      {
        if (!pushFrame(frame))
        {
          return false;
        }
        frameOffset = 0;
      }
    }

    return true;
  };

  auto refreshLiveStreamUrl = [&]() -> bool {
    if (!_isLive || !_refreshStreamUrl || shouldAbort())
    {
      return false;
    }

    auto refreshed = _refreshStreamUrl();
    if (!refreshed)
    {
      logging::warn("Failed to refresh live stream URL for '{}': {}", _title, refreshed.error());
      return false;
    }
    if (refreshed->empty())
    {
      logging::warn("Live refresh returned an empty stream URL for '{}'", _title);
      return false;
    }

    currentStreamUrl = std::move(*refreshed);
    logging::info("Refreshed live stream URL for '{}'", _title);
    return true;
  };

  while (!shouldAbort())
  {
    auto decoded = decoder.decode(currentStreamUrl, stopToken, onSamples);
    if (decoded)
    {
      if (!_isLive || shouldAbort())
      {
        break;
      }

      logging::info("Live stream URL ended for '{}', attempting refresh", _title);
      if (refreshLiveStreamUrl())
      {
        continue;
      }

      logging::warn("Stopping live playback for '{}' after refresh failure", _title);
      break;
    }

    if (shouldAbort())
    {
      logging::debug("In-process decoder stopped for '{}': {}", _title, decoded.error());
      break;
    }

    if (_isLive)
    {
      logging::warn(
        "Live decoder ended for '{}': {}. Attempting refresh.",
        _title,
        decoded.error());
      if (refreshLiveStreamUrl())
      {
        continue;
      }

      logging::warn("Stopping live playback for '{}' after refresh failure", _title);
      break;
    }

    logging::warn("In-process decoder failed for '{}': {}", _title, decoded.error());
    break;
  }

  if (frameOffset > 0 && !shouldAbort())
  {
    std::fill(frame.begin() + static_cast<std::ptrdiff_t>(frameOffset), frame.end(), 0);
    (void)pushFrame(frame);
  }

  _producerDone.store(true, std::memory_order_release);
  logging::info("Audio producer finished");
}

void AudioStreamer::consumerLoop()
{
  using Clock = std::chrono::steady_clock;

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

  // DPP handles 20ms pacing internally; we keep its send queue filled.
  _controller.configureVoiceClient();

  // 100 frames x 20ms = about 2s ahead.
  constexpr size_t TARGET_BUFFER_FRAMES = 100;

  std::vector<unsigned char> opusBuf(4000);
  bool stoppedEarly = false;
  size_t sendCount = 0;
  _playedAudio.store(false, std::memory_order_release);

  // Keep the estimated send-ahead near TARGET_BUFFER_FRAMES.
  auto playbackStart = Clock::now();

  while (!_shouldStop.load(std::memory_order_acquire) && !_controller.shouldStop())
  {
    if (_controller.isPaused())
    {
      if (!_controller.waitWhilePaused())
      {
        stoppedEarly = true;
        break;
      }
      sendCount = 0;
      playbackStart = Clock::now();
    }

    const auto elapsed = Clock::now() - playbackStart;
    const auto elapsedFrames = static_cast<size_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
      / FRAME_DURATION_MS);
    const size_t estimatedBuffered =
      (sendCount > elapsedFrames) ? (sendCount - elapsedFrames) : 0;

    if (estimatedBuffered >= TARGET_BUFFER_FRAMES)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_DURATION_MS));
      continue;
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
        "Audio watchdog: sends={}, ring={}, dppBuf~={}",
        sendCount,
        _ringBuffer.size(),
        estimatedBuffered);
    }
  }

  if (stoppedEarly || _shouldStop.load(std::memory_order_acquire) || _controller.shouldStop())
  {
    _controller.tryStopAudio();
    logging::info("Audio stopped early, cleared DPP buffer");
  }

  logging::info("Audio consumer finished");
}

}  // namespace audio