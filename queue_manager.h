#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dpp/snowflake.h>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

struct QueueItem
{
  std::string query;
  dpp::snowflake requesterId;
  dpp::snowflake guildId;
  dpp::snowflake channelId;
};

class MusicQueueManager
{
  std::deque<QueueItem> _queue;
  mutable std::shared_mutex _mutex;
  std::condition_variable_any _cv;
  std::condition_variable_any _pauseCv;
  std::atomic<bool> _isPlaying{false};
  std::atomic<bool> _shouldStopCurrent{false};
  std::atomic<bool> _isPaused{false};
  std::atomic<bool> _shutdown{false};
  std::atomic<bool> _disconnected{false};
  std::chrono::steady_clock::time_point _lastActivity;

public:
  MusicQueueManager() : _lastActivity(std::chrono::steady_clock::now())
  {}

  void resetForNewSession()
  {
    _disconnected.store(false, std::memory_order_release);
    _shouldStopCurrent.store(false, std::memory_order_release);
    _isPaused.store(false, std::memory_order_release);
  }

  void enqueue(QueueItem item)
  {
    {
      std::unique_lock lock(_mutex);
      _queue.push_back(std::move(item));
      _lastActivity = std::chrono::steady_clock::now();
    }
    _cv.notify_one();
  }

  [[nodiscard]] std::optional<QueueItem> dequeue()
  {
    std::unique_lock lock(_mutex);
    if (_queue.empty())
    {
      return std::nullopt;
    }
    auto item = std::move(_queue.front());
    _queue.pop_front();
    _lastActivity = std::chrono::steady_clock::now();
    return item;
  }

  [[nodiscard]] std::optional<QueueItem> waitForItem(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(_mutex);
    if (_cv.wait_for(lock, timeout, [this] {
          return !_queue.empty() || _shutdown.load(std::memory_order_acquire) || _disconnected.load(std::memory_order_acquire);
        }))
    {
      if (_queue.empty())
      {
        return std::nullopt;
      }
      auto item = std::move(_queue.front());
      _queue.pop_front();
      _lastActivity = std::chrono::steady_clock::now();
      return item;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool hasItems() const
  {
    std::shared_lock lock(_mutex);
    return !_queue.empty();
  }

  [[nodiscard]] size_t size() const
  {
    std::shared_lock lock(_mutex);
    return _queue.size();
  }

  void clear()
  {
    std::unique_lock lock(_mutex);
    _queue.clear();
  }

  void enqueueAtFront(QueueItem item)
  {
    {
      std::unique_lock lock(_mutex);
      _queue.push_front(std::move(item));
      _lastActivity = std::chrono::steady_clock::now();
    }
    _cv.notify_one();
  }

  void enqueueBatch(std::vector<QueueItem> items)
  {
    {
      std::unique_lock lock(_mutex);
      for (auto& item : items)
      {
        _queue.push_back(std::move(item));
      }
      _lastActivity = std::chrono::steady_clock::now();
    }
    _cv.notify_one();
  }

  void enqueueBatchAtFront(std::vector<QueueItem> items)
  {
    {
      std::unique_lock lock(_mutex);
      for (auto it = items.rbegin(); it != items.rend(); ++it)  // NOLINT(modernize-loop-convert)
      {
        _queue.push_front(std::move(*it));
      }
      _lastActivity = std::chrono::steady_clock::now();
    }
    _cv.notify_one();
  }

  void shuffle()
  {
    std::unique_lock lock(_mutex);
    if (_queue.size() <= 1)
    {
      return;
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::vector<QueueItem> vec(
      std::make_move_iterator(_queue.begin()), std::make_move_iterator(_queue.end()));
    std::ranges::shuffle(vec, gen);
    _queue.assign(std::make_move_iterator(vec.begin()), std::make_move_iterator(vec.end()));
    _lastActivity = std::chrono::steady_clock::now();
  }

  [[nodiscard]] std::vector<std::string> getQueueTitles(size_t maxItems = 10) const
  {
    std::shared_lock lock(_mutex);
    std::vector<std::string> titles;
    titles.reserve(std::min(maxItems, _queue.size()));
    size_t count = 0;
    for (const auto& item : _queue)
    {
      if (count++ >= maxItems)
      {
        break;
      }
      titles.push_back(item.query);
    }
    return titles;
  }

  [[nodiscard]] std::pair<std::vector<std::string>, size_t>
    getQueuePage(size_t page, size_t itemsPerPage = 10) const
  {
    std::shared_lock lock(_mutex);
    std::vector<std::string> items;
    const size_t totalPages = (_queue.size() + itemsPerPage - 1) / itemsPerPage;

    if (page < 1 || page > totalPages || _queue.empty())
    {
      return {items, totalPages};
    }

    const size_t startIdx = (page - 1) * itemsPerPage;
    const size_t endIdx = std::min(startIdx + itemsPerPage, _queue.size());

    items.reserve(endIdx - startIdx);
    for (size_t i = startIdx; i < endIdx; ++i)
    {
      items.push_back(_queue[i].query);
    }

    return {items, totalPages};
  }

  [[nodiscard]] bool remove(size_t position)
  {
    std::unique_lock lock(_mutex);
    if (position < 1 || position > _queue.size())
    {
      return false;
    }
    _queue.erase(_queue.begin() + static_cast<std::ptrdiff_t>(position - 1));
    _lastActivity = std::chrono::steady_clock::now();
    return true;
  }

  [[nodiscard]] size_t removeRange(size_t startPos, size_t endPos)
  {
    std::unique_lock lock(_mutex);
    if (startPos < 1 || startPos > _queue.size() || endPos < startPos)
    {
      return 0;
    }
    endPos = std::min(endPos, _queue.size());
    const size_t count = endPos - startPos + 1;
    auto startIt = _queue.begin() + static_cast<std::ptrdiff_t>(startPos - 1);
    auto endIt = _queue.begin() + static_cast<std::ptrdiff_t>(endPos);
    _queue.erase(startIt, endIt);
    _lastActivity = std::chrono::steady_clock::now();
    return count;
  }

  [[nodiscard]] bool move(size_t srcPos, size_t destPos)
  {
    std::unique_lock lock(_mutex);
    if (
      srcPos < 1 || srcPos > _queue.size() || destPos < 1 || destPos > _queue.size()
      || srcPos == destPos)
    {
      return false;
    }

    const size_t srcIdx = srcPos - 1;
    const size_t destIdx = destPos - 1;

    QueueItem item = std::move(_queue[srcIdx]);
    _queue.erase(_queue.begin() + static_cast<std::ptrdiff_t>(srcIdx));
    _queue.insert(_queue.begin() + static_cast<std::ptrdiff_t>(destIdx), std::move(item));
    _lastActivity = std::chrono::steady_clock::now();
    return true;
  }

  void setPlaying(bool playing)
  {
    _isPlaying.store(playing, std::memory_order_release);
    if (!playing)
    {
      std::unique_lock lock(_mutex);
      _lastActivity = std::chrono::steady_clock::now();
      _shouldStopCurrent.store(false, std::memory_order_release);
    }
  }

  [[nodiscard]] bool isPlaying() const
  {
    return _isPlaying.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool isIdle() const
  {
    std::shared_lock lock(_mutex);
    return _queue.empty() && !_isPlaying.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::chrono::steady_clock::time_point lastActivityTime() const
  {
    std::shared_lock lock(_mutex);
    return _lastActivity;
  }

  void updateActivity()
  {
    std::unique_lock lock(_mutex);
    _lastActivity = std::chrono::steady_clock::now();
  }

  void shutdown()
  {
    _shutdown.store(true, std::memory_order_release);
    _cv.notify_all();
    _pauseCv.notify_all();
  }

  void setDisconnected(bool disconnected)
  {
    _disconnected.store(disconnected, std::memory_order_release);
    _cv.notify_all();
    _pauseCv.notify_all();
  }

  void requestSkip()
  {
    _shouldStopCurrent.store(true, std::memory_order_release);
    _pauseCv.notify_all();
  }

  [[nodiscard]] bool shouldStop() const
  {
    return _shouldStopCurrent.load(std::memory_order_acquire);
  }

  void setPaused(bool paused)
  {
    _isPaused.store(paused, std::memory_order_release);
    if (!paused)
    {
      _pauseCv.notify_all();
    }
    std::unique_lock lock(_mutex);
    _lastActivity = std::chrono::steady_clock::now();
  }

  [[nodiscard]] bool isPaused() const noexcept
  {
    return _isPaused.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool waitWhilePaused()
  {
    if (!_isPaused.load(std::memory_order_acquire))
    {
      return true;
    }
    std::shared_lock lock(_mutex);
    _pauseCv.wait(lock, [this] {
      return !_isPaused.load(std::memory_order_acquire)
             || _shouldStopCurrent.load(std::memory_order_acquire)
             || _shutdown.load(std::memory_order_acquire)
             || _disconnected.load(std::memory_order_acquire);
    });
    return !_shouldStopCurrent.load(std::memory_order_acquire)
           && !_shutdown.load(std::memory_order_acquire)
           && !_disconnected.load(std::memory_order_acquire);
  }
};
