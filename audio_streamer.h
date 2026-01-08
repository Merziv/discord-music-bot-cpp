#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <dpp/discordvoiceclient.h>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <opus/opus.h>
#include <string>
#include <string_view>
#include <thread>

namespace audio {

inline constexpr int SAMPLE_RATE = 48000;
inline constexpr int CHANNELS = 2;
inline constexpr int FRAME_DURATION_MS = 20;
inline constexpr int SAMPLES_PER_CHANNEL = (SAMPLE_RATE / 1000) * FRAME_DURATION_MS;
inline constexpr int TOTAL_SAMPLES = SAMPLES_PER_CHANNEL * CHANNELS;
inline constexpr int FRAME_BYTES = TOTAL_SAMPLES * static_cast<int>(sizeof(int16_t));
inline constexpr size_t RING_BUFFER_FRAMES = 256;
inline constexpr int OPUS_BITRATE = 128000;

namespace logging {

enum class Level : std::uint8_t
{
  Debug,
  Info,
  Warn,
  Error
};

[[nodiscard]] inline std::string timestamp()
{
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto time = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
  localtime_r(&time, &tm);
  return std::format(
    "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
    tm.tm_year + 1900,
    tm.tm_mon + 1,
    tm.tm_mday,
    tm.tm_hour,
    tm.tm_min,
    tm.tm_sec,
    ms.count());
}

template<typename... Args>
void log(Level level, std::format_string<Args...> fmt, Args&&... args)
{
  static constexpr std::array levelNames = {"DEBUG", "INFO", "WARN", "ERROR"};
  const auto msg = std::format(fmt, std::forward<Args>(args)...);
  std::cerr << std::format("[{}] [{}] {}\n", timestamp(), levelNames[static_cast<int>(level)], msg);
}

template<typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args)
{
  log(Level::Debug, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args)
{
  log(Level::Info, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args)
{
  log(Level::Warn, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args)
{
  log(Level::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace logging

template<typename T, size_t Capacity>
class LockFreeRingBuffer
{
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

  std::array<T, Capacity> _buffer{};
  alignas(64) std::atomic<size_t> _head{0};
  alignas(64) std::atomic<size_t> _tail{0};

public:
  [[nodiscard]] bool push(const T& item) noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & (Capacity - 1);

    if (next == _tail.load(std::memory_order_acquire))
    {
      return false;
    }

    _buffer[head] = item;
    _head.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool push(T&& item) noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & (Capacity - 1);

    if (next == _tail.load(std::memory_order_acquire))
    {
      return false;
    }

    _buffer[head] = std::move(item);
    _head.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::optional<T> pop() noexcept
  {
    const size_t tail = _tail.load(std::memory_order_relaxed);

    if (tail == _head.load(std::memory_order_acquire))
    {
      return std::nullopt;
    }

    T item = std::move(_buffer[tail]);
    _tail.store((tail + 1) & (Capacity - 1), std::memory_order_release);
    return item;
  }

  [[nodiscard]] size_t size() const noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t tail = _tail.load(std::memory_order_relaxed);
    return (head - tail + Capacity) & (Capacity - 1);
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return _head.load(std::memory_order_relaxed) == _tail.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool full() const noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & (Capacity - 1);
    return next == _tail.load(std::memory_order_relaxed);
  }

  void clear() noexcept
  {
    _tail.store(_head.load(std::memory_order_relaxed), std::memory_order_release);
  }
};

struct PlaybackController
{
  std::function<bool()> shouldStop;
  std::function<bool()> isPaused;
  std::function<bool()> waitWhilePaused;
};

class AudioStreamer
{
  using AudioFrame = std::array<int16_t, TOTAL_SAMPLES>;
  using RingBuffer = LockFreeRingBuffer<AudioFrame, RING_BUFFER_FRAMES>;

  dpp::discord_voice_client* _voiceClient;
  std::string _streamUrl;
  std::string _title;
  PlaybackController _controller;

  RingBuffer _ringBuffer;
  std::atomic<bool> _producerDone{false};
  std::atomic<bool> _shouldStop{false};

  std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> _encoder{
    nullptr, opus_encoder_destroy};

  std::jthread _producerThread;

public:
  AudioStreamer(
    dpp::discord_voice_client* voiceClient,
    std::string streamUrl,
    std::string title,
    PlaybackController controller);

  ~AudioStreamer();

  AudioStreamer(const AudioStreamer&) = delete;
  AudioStreamer& operator=(const AudioStreamer&) = delete;
  AudioStreamer(AudioStreamer&&) = delete;
  AudioStreamer& operator=(AudioStreamer&&) = delete;

  void start();
  void stop();

private:
  void initEncoder();
  void producerLoop(const std::stop_token& stopToken);
  void consumerLoop();

  [[nodiscard]] static std::string makeTempLogPath(const char* prefix);
  [[nodiscard]] static std::string quote(std::string_view str);
};

}  // namespace audio
