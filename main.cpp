#include "audio_streamer.h"
#include "config_manager.h"
#include "extractor.h"
#include "queue_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <dpp/cluster.h>
#include <dpp/discordclient.h>
#include <dpp/discordvoiceclient.h>
#include <dpp/dpp.h>
#include <dpp/intents.h>
#include <dpp/message.h>
#include <dpp/misc-enum.h>
#include <dpp/presence.h>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace logging = audio::logging;

[[nodiscard]] std::string formatDuration(std::chrono::steady_clock::duration dur)
{
  using namespace std::chrono;
  const auto hrs = duration_cast<hours>(dur);
  const auto mins = duration_cast<minutes>(dur) % 60;
  const auto secs = duration_cast<seconds>(dur) % 60;

  if (hrs.count() > 0)
  {
    return std::format("{}h {}m {}s", hrs.count(), mins.count(), secs.count());
  }
  if (mins.count() > 0)
  {
    return std::format("{}m {}s", mins.count(), secs.count());
  }
  return std::format("{}s", secs.count());
}

namespace {
std::atomic<bool> shutdownRequested{false};

MusicQueueManager musicQueue;

std::unique_ptr<dpp::cluster> bot;

std::atomic<uint64_t> currentGuildId{0};
std::atomic<bool> voiceConnected{false};

std::chrono::steady_clock::time_point botStartTime;
std::atomic<std::chrono::steady_clock::time_point> voiceSessionStart{};
std::atomic<std::chrono::steady_clock::duration> totalPlayTime{
  std::chrono::steady_clock::duration::zero()};
std::atomic<bool> isCurrentlyPlaying{false};
std::chrono::steady_clock::time_point currentTrackStart{};
std::mutex playTimeMutex;
std::string currentTrackTitle;
std::jthread voiceSessionThread;
std::atomic<uint64_t> voiceSessionGeneration{0};

std::mutex voiceClientMutex;
dpp::discord_voice_client* activeVoiceClient{nullptr};

std::mutex voiceSessionMutex;

std::atomic<uint64_t> activeResponseChannel{0};
std::atomic<bool> initialReadyReceived{false};

// Tracked disconnect threads — joined at shutdown to prevent use-after-free on bot.
std::mutex disconnectThreadsMutex;
struct TrackedThread
{
  std::jthread thread;
  std::shared_ptr<std::atomic<bool>> done;
};
std::vector<TrackedThread> disconnectThreads;

void scheduleDisconnect(dpp::snowflake gId, uint64_t expectedGeneration)
{
  if (!bot)
  {
    return;
  }
  auto* cluster = bot.get();
  std::lock_guard lock(disconnectThreadsMutex);
  // Prune completed threads to avoid unbounded growth.
  for (auto it = disconnectThreads.begin(); it != disconnectThreads.end(); )
  {
    if (it->done->load(std::memory_order_acquire))
    {
      if (it->thread.joinable())
      {
        it->thread.join();
      }
      it = disconnectThreads.erase(it);
    }
    else
    {
      ++it;
    }
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  disconnectThreads.push_back(TrackedThread{
    .thread = std::jthread([gId, expectedGeneration, cluster, done] {
      if (voiceSessionGeneration.load(std::memory_order_acquire) != expectedGeneration)
      {
        done->store(true, std::memory_order_release);
        return;
      }
      if (auto* shard = cluster->get_shard(0))
      {
        shard->disconnect_voice(gId);
      }
      done->store(true, std::memory_order_release);
    }),
    .done = done,
  });
}

void cleanupStaleVoice(std::string_view reason)
{
  const auto generation = voiceSessionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

  // Use exchange so only one caller performs the cleanup
  if (!voiceConnected.exchange(false, std::memory_order_acq_rel))
  {
    return;
  }
  logging::warn("Cleaning up stale voice state: {}", reason);
  {
    std::lock_guard lock(voiceClientMutex);
    activeVoiceClient = nullptr;
  }
  musicQueue.setDisconnected(true);
  musicQueue.requestSkip();

  // Disconnect on a separate thread — disconnect_voice can re-enter
  // on_voice_ready on the same DPP thread, which would deadlock.
  auto gId = dpp::snowflake(currentGuildId.load(std::memory_order_acquire));
  scheduleDisconnect(gId, generation);
}

[[nodiscard]] inline const config::BotConfig& botConfig()
{
  return config::getConfig().bot();
}

[[nodiscard]] inline bool isCommandChannel(dpp::snowflake id)
{
  const auto& ids = botConfig().commandChannelIds;
  const auto& constId = std::as_const(id);
  return std::ranges::find(ids, static_cast<uint64_t>(constId)) != ids.end();
}

[[nodiscard]] inline dpp::snowflake responseChannelFor(dpp::snowflake sourceChannel)
{
  const auto configured = botConfig().responseChannelId;
  return configured != 0 ? dpp::snowflake(configured) : sourceChannel;
}

[[nodiscard]] inline dpp::snowflake responseChannel()
{
  return dpp::snowflake(activeResponseChannel.load(std::memory_order_acquire));
}

[[nodiscard]] inline bool isActiveVoiceSession(uint64_t expectedGeneration)
{
  return voiceConnected.load(std::memory_order_acquire)
         && voiceSessionGeneration.load(std::memory_order_acquire) == expectedGeneration;
}

}  // namespace

void signalHandler(int /*signal*/)
{
  shutdownRequested.store(true, std::memory_order_release);
}

void streamAudio(const QueueItem& item, uint64_t sessionGeneration)
{
  logging::info("Starting streamAudio. Query: {}", item.query);
  musicQueue.setPlaying(true);

  auto trackStart = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(playTimeMutex);
    currentTrackStart = trackStart;
    isCurrentlyPlaying.store(true, std::memory_order_release);
  }

  bool played = false;
  bool hasLoggedNowPlaying = false;

  std::vector<std::string> candidates;
  bool searchFailed = false;
  try
  {
    candidates = searchCandidateUrls(item.query, shutdownRequested);
  }
  catch (const std::exception& e)
  {
    logging::error("Failed to search candidates for query '{}': {}", item.query, e.what());
    searchFailed = true;
  }

  if (searchFailed && bot)
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content(std::format(
          "\u274c Search failed for `{}`. This may be a network issue or an "
          "unsupported query format. Please try again.",
          item.query)));
  }

  for (size_t ci = 0; ci < candidates.size() && !played; ++ci)
  {
    if (shutdownRequested.load(std::memory_order_acquire)
        || musicQueue.shouldStop() || !isActiveVoiceSession(sessionGeneration))
    {
      break;
    }

    const auto& videoUrl = candidates[ci];

    auto result = extractStreamInfo(videoUrl, shutdownRequested);
    if (!result)
    {
      logging::warn("Candidate {}/{} failed: {}", ci + 1, candidates.size(), result.error());
      continue;
    }

    const auto& info = *result;

    if (!hasLoggedNowPlaying)
    {
      hasLoggedNowPlaying = true;
      logging::info("Now playing: '{}'", info.title);
      {
        std::lock_guard lock(playTimeMutex);
        currentTrackTitle = info.title;
      }
      if (bot)
      {
        std::string presenceText = info.title;
        if (presenceText.size() > 120)
        {
          presenceText = presenceText.substr(0, 117) + "...";
        }
        bot->set_presence(dpp::presence(dpp::ps_online, dpp::at_listening, presenceText));
      }

      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content(std::format(
                              "🎵 Now playing: '{}'\n{} {}",
                              info.title,
                              info.webpageUrl,
                              botConfig().botReactionImage)));
    }
    else
    {
      logging::info("Trying candidate {}/{}: '{}'", ci + 1, candidates.size(), info.title);
    }

    logging::debug(
      "Stream URL (truncated): {}...",
      info.streamUrl.substr(0, std::min(info.streamUrl.size(), size_t{128})));

    try
    {
      const std::string liveRefreshUrl = info.webpageUrl;
      audio::PlaybackController controller{
        .shouldStop = [sessionGeneration] {
          return musicQueue.shouldStop() || !isActiveVoiceSession(sessionGeneration);
        },
        .isPaused = [] { return musicQueue.isPaused(); },
        .waitWhilePaused = [] { return musicQueue.waitWhilePaused(); },
        .trySendAudioOpus = [sessionGeneration](const uint8_t* data, size_t len) -> bool {
          std::lock_guard lock(voiceClientMutex);
          if (!activeVoiceClient || !isActiveVoiceSession(sessionGeneration))
          {
            return false;
          }
          activeVoiceClient->send_audio_opus(data, len, 20, false);
          return true;
        },
        .tryStopAudio = [sessionGeneration] {
          std::lock_guard lock(voiceClientMutex);
          if (activeVoiceClient && isActiveVoiceSession(sessionGeneration))
          {
            activeVoiceClient->stop_audio();
          }
        },
        .isReady = [sessionGeneration]() -> bool {
          std::lock_guard lock(voiceClientMutex);
          return activeVoiceClient != nullptr
                 && isActiveVoiceSession(sessionGeneration)
                 && activeVoiceClient->is_ready();
        },
        .configureVoiceClient = [sessionGeneration] {
          std::lock_guard lock(voiceClientMutex);
          if (activeVoiceClient && isActiveVoiceSession(sessionGeneration))
          {
            activeVoiceClient->set_timescale(1000000);
            activeVoiceClient->speak();
            logging::info("Voice client configured: timescale=1000000, using recorded audio pacing mode");
          }
        },
      };
      auto refreshLiveStreamUrl = [liveRefreshUrl]() -> std::expected<std::string, std::string> {
        auto refreshed = extractStreamInfo(liveRefreshUrl, shutdownRequested);
        if (!refreshed)
        {
          return std::unexpected(refreshed.error());
        }
        return refreshed->streamUrl;
      };

      audio::AudioStreamer streamer(
        info.streamUrl,
        info.title,
        info.isLive,
        refreshLiveStreamUrl,
        std::move(controller));
      streamer.start();

      if (streamer.playedAudio())
      {
        played = true;
        break;
      }

      logging::warn("No audio produced for '{}', trying next candidate", info.title);
    }
    catch (const std::exception& e)
    {
      logging::error("Audio streaming error: {}", e.what());
    }
  }

  if (!played && !musicQueue.shouldStop() && isActiveVoiceSession(sessionGeneration))
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content("❌ Failed to play track after multiple attempts"));
  }

  {
    std::lock_guard lock(playTimeMutex);
    auto trackDuration = std::chrono::steady_clock::now() - trackStart;
    totalPlayTime.store(
      totalPlayTime.load(std::memory_order_relaxed) + trackDuration, std::memory_order_release);
    isCurrentlyPlaying.store(false, std::memory_order_release);
  }

  musicQueue.setPlaying(false);
  {
    std::lock_guard lock(playTimeMutex);
    currentTrackTitle.clear();
  }
  if (bot)
  {
    bot->set_presence(
      dpp::presence(dpp::ps_online, dpp::at_game, std::string(botConfig().statusPlayingGame)));
  }
}

void voiceSessionLoop(const dpp::snowflake guildId, uint64_t sessionGeneration)
{
  logging::info("Voice session started for guild {}", static_cast<uint64_t>(guildId));

  voiceSessionStart.store(std::chrono::steady_clock::now(), std::memory_order_release);
  totalPlayTime.store(std::chrono::steady_clock::duration::zero(), std::memory_order_release);

  try
  {
  while (!shutdownRequested.load(std::memory_order_acquire)
         && voiceConnected.load(std::memory_order_acquire))
  {
    auto item = musicQueue.waitForItem(std::chrono::seconds(30));

    if (item)
    {
      streamAudio(*item, sessionGeneration);
      continue;
    }

    if (!voiceConnected.load(std::memory_order_acquire))
    {
      logging::info("Voice connection lost, exiting session loop");
      break;
    }

    auto idleTime = std::chrono::steady_clock::now() - musicQueue.lastActivityTime();
    if (idleTime > botConfig().idleTimeout && musicQueue.isIdle())
    {
      logging::info("Idle timeout reached, disconnecting from voice");
      bot->message_create(
        dpp::message()
          .set_channel_id(responseChannel())
          .set_content(
            std::format("👋 Disconnecting due to inactivity {}", botConfig().botReactionImage)));
      break;
    }
  }
  }
  catch (const std::exception& e)
  {
    logging::error("Uncaught exception in voice session loop: {}", e.what());
  }
  catch (...)
  {
    logging::error("Unknown exception in voice session loop");
  }

  if (voiceSessionGeneration.load(std::memory_order_acquire) != sessionGeneration)
  {
    logging::info("Voice session {} superseded before teardown", sessionGeneration);
    return;
  }

  if (voiceConnected.exchange(false, std::memory_order_acq_rel))
  {
    musicQueue.setDisconnected(true);

    auto disconnectGuildId = dpp::snowflake(currentGuildId.load(std::memory_order_acquire));
    logging::debug(
      "Requesting voice disconnect, guild: {}",
      static_cast<uint64_t>(std::as_const(disconnectGuildId)));

    // Null out pointer first — DPP destroys the voice client during disconnect.
    {
      std::lock_guard lock(voiceClientMutex);
      activeVoiceClient = nullptr;
    }

    // Schedule disconnect on a separate thread so this thread can exit
    // before on_voice_ready tries to join it (avoids deadlock).
    scheduleDisconnect(disconnectGuildId, sessionGeneration);
  }
  if (bot)
  {
    bot->set_presence(
      dpp::presence(dpp::ps_online, dpp::at_game, std::string(botConfig().statusPlayingGame)));
  }
  logging::info("Voice session ended");
}

[[nodiscard]] std::expected<std::string, std::string> sanitizeQuery(std::string_view input)
{
  auto start = input.find_first_not_of(" \t\n\r");
  if (start == std::string_view::npos)
  {
    return std::unexpected(
      std::format("Usage: `{}play <YouTube URL or search query>`", botConfig().commandPrefix));
  }
  auto end = input.find_last_not_of(" \t\n\r");
  input = input.substr(start, end - start + 1);

  constexpr size_t MAX_QUERY_LENGTH = 500;
  if (input.size() > MAX_QUERY_LENGTH)
  {
    return std::unexpected(std::string("\u274c Query is too long (max 500 characters)"));
  }

  for (char c : input)
  {
    if (c == '\0' || (static_cast<unsigned char>(c) < 0x20 && c != ' ' && c != '\t'))
    {
      return std::unexpected(std::string("\u274c Query contains invalid characters"));
    }
  }

  if (input.starts_with('-'))
  {
    return std::unexpected(std::string("\u274c Invalid query"));
  }

  if (input.starts_with('/') || input.starts_with("file://"))
  {
    return std::unexpected(std::string("\u274c Local file paths are not supported"));
  }

  return std::string(input);
}

void handlePlayCommand(
  const dpp::message_create_t& event, std::string_view rawQuery, bool addToFront = false)
{
  const auto& msg = event.msg;

  auto sanitized = sanitizeQuery(rawQuery);
  if (!sanitized)
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content(sanitized.error()));
    return;
  }
  const auto& query = *sanitized;

  logging::info("Received play request: {}", query);

  auto* guild = dpp::find_guild(msg.guild_id);
  if (guild == nullptr)
  {
    logging::error("Guild not found");
    return;
  }

  auto it = guild->voice_members.find(msg.author.id);
  if (it == guild->voice_members.end())
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content("❌ You must be in a voice channel to use this command"));
    return;
  }

  const auto channelId = it->second.channel_id;

  if (auto playlist = extractPlaylistInfo(query))
  {
    if (playlist->videoUrls.empty())
    {
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content("❌ Playlist is empty or couldn't be loaded"));
      return;
    }

    std::vector<QueueItem> items;
    items.reserve(playlist->videoUrls.size());

    for (const auto& url : playlist->videoUrls)
    {
      items.push_back(QueueItem{
        .query = url,
        .requesterId = msg.author.id,
        .guildId = msg.guild_id,
        .channelId = channelId,
      });
    }

    const size_t count = items.size();

    if (voiceConnected.load(std::memory_order_acquire))
    {
      if (addToFront)
      {
        musicQueue.enqueueBatchAtFront(std::move(items));
      }
      else
      {
        musicQueue.enqueueBatch(std::move(items));
      }
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content(std::format(
                              "📝 Added {} tracks from '{}' to {}",
                              count,
                              playlist->playlistTitle,
                              addToFront ? "front of queue" : "queue")));
    }
    else
    {
      if (addToFront)
      {
        musicQueue.enqueueBatchAtFront(std::move(items));
      }
      else
      {
        musicQueue.enqueueBatch(std::move(items));
      }

      if (auto* shard = event.from())
      {
        currentGuildId.store(static_cast<uint64_t>(msg.guild_id), std::memory_order_release);
        logging::debug("Initiating voice connection");
        shard->connect_voice(msg.guild_id, channelId, false, false, true);
      }

      bot->message_create(
        dpp::message()
          .set_channel_id(responseChannel())
          .set_content(std::format(
            "🎵 Starting playlist '{}' with {} tracks", playlist->playlistTitle, count)));
    }
    return;
  }

  QueueItem item{
    .query = std::string(query),
    .requesterId = msg.author.id,
    .guildId = msg.guild_id,
    .channelId = channelId,
  };

  if (voiceConnected.load(std::memory_order_acquire))
  {
    if (addToFront)
    {
      musicQueue.enqueueAtFront(std::move(item));
      bot->message_create(
        dpp::message().set_channel_id(responseChannel()).set_content("📝 Added to front of queue"));
    }
    else
    {
      musicQueue.enqueue(std::move(item));
      bot->message_create(
        dpp::message()
          .set_channel_id(responseChannel())
          .set_content(std::format("📝 Added to queue (position {})", musicQueue.size())));
    }
  }
  else
  {
    musicQueue.enqueue(std::move(item));

    if (auto* shard = event.from())
    {
      currentGuildId.store(static_cast<uint64_t>(msg.guild_id), std::memory_order_release);
      logging::debug("Initiating voice connection");
      shard->connect_voice(msg.guild_id, channelId, false, false, true);
    }
  }
}

void handleSkipCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("❌ Nothing is playing"));
    return;
  }

  musicQueue.requestSkip();
  bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content("⏭️ Skipping..."));
}

void handleQueueCommand(const dpp::message_create_t& /*event*/, std::string_view args)
{
  const size_t queueSize = musicQueue.size();
  if (queueSize == 0 && !musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("📭 Queue is empty"));
    return;
  }

  size_t page = 1;
  if (!args.empty())
  {
    try
    {
      auto parsed = std::stoull(std::string(args));
      page = (parsed < 1) ? 1 : parsed;
    }
    catch (...)
    {
      page = 1;
    }
  }

  constexpr size_t ITEMS_PER_PAGE = 10;
  auto [items, totalPages] = musicQueue.getQueuePage(page, ITEMS_PER_PAGE);

  if (items.empty() && page > 1)
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content(std::format("❌ Page {} doesn't exist (max: {})", page, totalPages)));
    return;
  }

  std::string content;
  content.reserve(1024);

  content += std::format(
    "📋 **Queue** ({} items){}\n",
    queueSize,
    musicQueue.isPlaying() ? " - 🎵 Currently playing" : "");

  const size_t startIdx = (page - 1) * ITEMS_PER_PAGE;
  for (size_t i = 0; i < items.size(); ++i)
  {
    std::string_view query = items[i];
    if (query.size() > 60)
    {
      content += std::format("**{}**. {}...\n", startIdx + i + 1, query.substr(0, 57));
    }
    else
    {
      content += std::format("**{}**. {}\n", startIdx + i + 1, query);
    }
  }

  if (totalPages > 1)
  {
    content += std::format(
      "\n📄 Page {}/{} • Use `{}queue <page>` to see more",
      page,
      totalPages,
      botConfig().commandPrefix);
  }

  bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content(content));
}

void handleStopCommand(const dpp::message_create_t& /*event*/)
{
  musicQueue.clear();
  musicQueue.setPaused(false);
  musicQueue.requestSkip();

  bot->message_create(dpp::message()
                        .set_channel_id(responseChannel())
                        .set_content("⏹️ Stopped playback and cleared queue"));
}

void handlePauseCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("❌ Nothing is playing"));
    return;
  }

  if (musicQueue.isPaused())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("⏸️ Already paused"));
    return;
  }

  musicQueue.setPaused(true);
  bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content("⏸️ Paused"));
}

void handleResumeCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("❌ Nothing is playing"));
    return;
  }

  if (!musicQueue.isPaused())
  {
    bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content("▶️ Not paused"));
    return;
  }

  musicQueue.setPaused(false);
  bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content("▶️ Resumed"));
}

void handleClearCommand(const dpp::message_create_t& /*event*/)
{
  const size_t cleared = musicQueue.size();
  musicQueue.clear();

  if (cleared == 0)
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("📭 Queue was already empty"));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content(std::format("🗑️ Cleared {} items from queue", cleared)));
  }
}

void handlePlayTopCommand(const dpp::message_create_t& event, std::string_view query)
{
  handlePlayCommand(event, query, true);
}

void handleShuffleCommand(const dpp::message_create_t& /*event*/)
{
  const size_t queueSize = musicQueue.size();

  if (queueSize < 2)
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content("❌ Need at least 2 items in queue to shuffle"));
    return;
  }

  musicQueue.shuffle();
  bot->message_create(dpp::message()
                        .set_channel_id(responseChannel())
                        .set_content(std::format("🔀 Shuffled {} items in queue", queueSize)));
}

void handleUptimeCommand(const dpp::message_create_t& /*event*/)
{
  const auto now = std::chrono::steady_clock::now();
  const auto botUptime = now - botStartTime;

  std::string content = std::format("⏱️ **Bot uptime:** {}", formatDuration(botUptime));

  if (voiceConnected.load(std::memory_order_acquire))
  {
    const auto sessionStart = voiceSessionStart.load(std::memory_order_acquire);
    const auto sessionDuration = now - sessionStart;

    auto totalPlay = totalPlayTime.load(std::memory_order_acquire);
    if (isCurrentlyPlaying.load(std::memory_order_acquire))
    {
      std::lock_guard lock(playTimeMutex);
      totalPlay += now - currentTrackStart;
    }

    content += std::format(
      "\n🎵 **Voice session:** {} (playing: {})",
      formatDuration(sessionDuration),
      formatDuration(totalPlay));
  }

  bot->message_create(dpp::message().set_channel_id(responseChannel()).set_content(content));
}

void handleRemoveCommand(const dpp::message_create_t& /*event*/, std::string_view args)
{
  if (args.empty())
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content(std::format(
                            "Usage: `{}remove <position>` or `{}remove <start> <end>` for range",
                            botConfig().commandPrefix,
                            botConfig().commandPrefix)));
    return;
  }

  size_t startPos = 0;
  size_t endPos = 0;

  const auto spacePos = args.find(' ');
  if (spacePos != std::string_view::npos)
  {
    try
    {
      startPos = std::stoull(std::string(args.substr(0, spacePos)));
      endPos = std::stoull(std::string(args.substr(spacePos + 1)));
    }
    catch (...)
    {
      bot->message_create(
        dpp::message().set_channel_id(responseChannel()).set_content("❌ Invalid range"));
      return;
    }

    const size_t removed = musicQueue.removeRange(startPos, endPos);
    if (removed == 0)
    {
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content("❌ Invalid range or queue is empty"));
    }
    else
    {
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content(std::format(
                              "🗑️ Removed {} items (positions {}-{})", removed, startPos, endPos)));
    }
  }
  else
  {
    try
    {
      startPos = std::stoull(std::string(args));
    }
    catch (...)
    {
      bot->message_create(
        dpp::message().set_channel_id(responseChannel()).set_content("❌ Invalid position"));
      return;
    }

    if (musicQueue.remove(startPos))
    {
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content(std::format("🗑️ Removed item at position {}", startPos)));
    }
    else
    {
      bot->message_create(dpp::message()
                            .set_channel_id(responseChannel())
                            .set_content("❌ Invalid position or queue is empty"));
    }
  }
}

void handleMoveCommand(const dpp::message_create_t& /*event*/, std::string_view args)
{
  if (args.empty())
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content(std::format("Usage: `{}move <from> <to>`", botConfig().commandPrefix)));
    return;
  }

  const auto spacePos = args.find(' ');
  if (spacePos == std::string_view::npos)
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content(std::format("Usage: `{}move <from> <to>`", botConfig().commandPrefix)));
    return;
  }

  size_t fromPos = 0;
  size_t toPos = 0;

  try
  {
    fromPos = std::stoull(std::string(args.substr(0, spacePos)));
    toPos = std::stoull(std::string(args.substr(spacePos + 1)));
  }
  catch (...)
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("❌ Invalid positions"));
    return;
  }

  if (musicQueue.move(fromPos, toPos))
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(responseChannel())
        .set_content(std::format("↕️ Moved item from position {} to {}", fromPos, toPos)));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content("❌ Invalid positions or queue is empty"));
  }
}

void handleNowPlayingCommand(const dpp::message_create_t& /*event*/)
{
  bool playing = isCurrentlyPlaying.load(std::memory_order_acquire);
  std::string title;
  std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::duration::zero();

  {
    std::lock_guard lock(playTimeMutex);
    title = currentTrackTitle;
    if (playing)
    {
      elapsed = std::chrono::steady_clock::now() - currentTrackStart;
    }
  }

  if (title.empty())
  {
    auto next = musicQueue.getQueueTitles(1);
    if (!next.empty())
    {
      title = next.front();
    }
  }

  if (title.empty())
  {
    bot->message_create(
      dpp::message().set_channel_id(responseChannel()).set_content("📭 Nothing is playing or queued"));
    return;
  }

  if (playing)
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content(std::format(
                            "🎧 Now playing: '{}' • elapsed: {}", title, formatDuration(elapsed))));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(responseChannel())
                          .set_content(std::format("🎧 Next: '{}'", title)));
  }
}

int main(int argc, char* argv[])
{
  std::string configPath = "config.json";
  if (argc > 1)
  {
    configPath = argv[1];
  }

  auto configResult = config::ConfigManager::loadFromFile(configPath);
  if (!configResult)
  {
    std::cerr << "Failed to load config: " << configResult.error() << "\n";
    return 1;
  }
  config::setGlobalConfig(std::move(*configResult));

  // Guard against LogLevel enum drift between config and audio namespaces.
  static_assert(
    static_cast<int>(config::LogLevel::Debug) == static_cast<int>(audio::logging::Level::Debug)
    && static_cast<int>(config::LogLevel::Info) == static_cast<int>(audio::logging::Level::Info)
    && static_cast<int>(config::LogLevel::Warn) == static_cast<int>(audio::logging::Level::Warn)
    && static_cast<int>(config::LogLevel::Error) == static_cast<int>(audio::logging::Level::Error),
    "config::LogLevel and audio::logging::Level enumerators must match");
  logging::setMinLevel(
    static_cast<audio::logging::Level>(static_cast<int>(botConfig().logLevel)));

  logging::info("Config loaded from: {}", configPath);

  // Prefer the dedicated response channel fall back to the first command channel.
  if (botConfig().responseChannelId != 0)
  {
    activeResponseChannel.store(botConfig().responseChannelId, std::memory_order_relaxed);
  }
  else if (!botConfig().commandChannelIds.empty())
  {
    activeResponseChannel.store(
      botConfig().commandChannelIds.front(), std::memory_order_relaxed);
  }

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGPIPE, SIG_IGN);   // Return EPIPE instead of killing process
  // Auto-reap children; waitpid in producerLoop handles ECHILD gracefully.
  std::signal(SIGCHLD, SIG_IGN);

  botStartTime = std::chrono::steady_clock::now();

  bot = std::make_unique<dpp::cluster>(
    botConfig().botToken, dpp::i_default_intents | dpp::i_message_content);

  bot->on_log([](const dpp::log_t& event) {
    switch (event.severity)
    {
    case dpp::ll_trace:
    case dpp::ll_debug:
      logging::debug("[DPP] {}", event.message);
      break;
    case dpp::ll_info:
      logging::info("[DPP] {}", event.message);
      break;
    case dpp::ll_warning:
      logging::warn("[DPP] {}", event.message);
      break;
    case dpp::ll_error:
    case dpp::ll_critical:
      logging::error("[DPP] {}", event.message);
      break;
    }
  });

  bot->on_ready([](const dpp::ready_t&) {
    logging::info("{}", botConfig().startupMessage);
    bot->set_presence(
      dpp::presence(dpp::ps_online, dpp::at_game, std::string(botConfig().statusPlayingGame)));

    // After a full gateway reset (not the initial connection),
    // any existing voice session is invalidated by Discord.
    if (initialReadyReceived.exchange(true, std::memory_order_acq_rel))
    {
      if (voiceConnected.load(std::memory_order_acquire))
      {
        cleanupStaleVoice("gateway sent new READY (session was not resumable)");
      }
    }
  });

  bot->on_resumed([](const dpp::resumed_t&) {
    logging::info("Gateway session resumed successfully");
  });

  bot->on_message_create([](const dpp::message_create_t& event) {
    const auto& msg = event.msg;

    if (!isCommandChannel(msg.channel_id))
    {
      return;
    }

    if (
      msg.author.id == bot->me.id || msg.content.empty()
      || !msg.content.starts_with(botConfig().commandPrefix))
    {
      return;
    }

    const dpp::snowflake respChannel = responseChannelFor(msg.channel_id);
    activeResponseChannel.store(
      static_cast<uint64_t>(std::as_const(respChannel)), std::memory_order_release);

    musicQueue.updateActivity();

    const auto spacePos = msg.content.find(' ');
    const auto cmd = msg.content.substr(0, spacePos);
    const auto args = (spacePos != std::string::npos) ? msg.content.substr(spacePos + 1) : "";

    const auto prefix = std::string(botConfig().commandPrefix);
    auto isCmd = [&](std::initializer_list<std::string_view> names) {
      return std::ranges::any_of(
        names, [&](std::string_view name) { return cmd == prefix + std::string(name); });
    };

    if (isCmd({"play", "p"}))
    {
      handlePlayCommand(event, args);
    }
    else if (isCmd({"playtop", "ptop"}))
    {
      handlePlayTopCommand(event, args);
    }
    else if (isCmd({"skip"}))
    {
      handleSkipCommand(event);
    }
    else if (isCmd({"queue", "q"}))
    {
      handleQueueCommand(event, args);
    }
    else if (isCmd({"clear"}))
    {
      handleClearCommand(event);
    }
    else if (isCmd({"shuffle"}))
    {
      handleShuffleCommand(event);
    }
    else if (isCmd({"stop"}))
    {
      handleStopCommand(event);
    }
    else if (isCmd({"pause"}))
    {
      handlePauseCommand(event);
    }
    else if (isCmd({"resume", "unpause"}))
    {
      handleResumeCommand(event);
    }
    else if (isCmd({"uptime"}))
    {
      handleUptimeCommand(event);
    }
    else if (isCmd({"remove", "rm"}))
    {
      handleRemoveCommand(event, args);
    }
    else if (isCmd({"move", "mv"}))
    {
      handleMoveCommand(event, args);
    }
    else if (isCmd({"nowplaying", "np"}))
    {
      handleNowPlayingCommand(event);
    }
  });

  bot->on_voice_ready([](const dpp::voice_ready_t& event) {
    if (auto* vc = event.voice_client)
    {
      const auto newGeneration = voiceSessionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

      // Tear down previous session before locking — avoids deadlock
      // if the old loop's disconnect_voice re-enters on_voice_ready.
      voiceConnected.store(false, std::memory_order_release);
      {
        std::lock_guard lock(voiceClientMutex);
        activeVoiceClient = nullptr;
      }
      musicQueue.setDisconnected(true);
      musicQueue.requestSkip();

      // Session mutex prevents concurrent on_voice_ready races.
      {
        std::lock_guard sessionLock(voiceSessionMutex);
        if (voiceSessionThread.joinable())
        {
          voiceSessionThread.join();
        }

        // Now set up the new session
        {
          std::lock_guard lock(voiceClientMutex);
          activeVoiceClient = vc;
        }
        voiceConnected.store(true, std::memory_order_release);
        logging::info("Voice client set (ptr={})", static_cast<void*>(vc));

        musicQueue.resetForNewSession();
        auto guildId = dpp::snowflake(currentGuildId.load(std::memory_order_acquire));
        voiceSessionThread = std::jthread(voiceSessionLoop, guildId, newGeneration);
      }
    }
  });

  bot->on_voice_state_update([](const dpp::voice_state_update_t& event) {
    if (event.state.user_id == bot->me.id)
    {
      if (!event.state.channel_id)
      {
        logging::info("Bot disconnected from voice channel");
        cleanupStaleVoice("bot left voice channel");
      }
      else
      {
        // Bot connected or moved to a channel
        // Could reset disconnected, for now on_voice_ready handles connection
      }
    }
  });

  bot->start(dpp::st_return);

  bool gatewayRecoveryActive = false;
  bool restartForGatewayStall = false;
  std::chrono::steady_clock::time_point gatewayRecoveryStart{};

  while (!shutdownRequested.load(std::memory_order_acquire))
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto* shard = bot ? bot->get_shard(0) : nullptr;
    const bool shardConnected = shard != nullptr && shard->is_connected();

    if (!initialReadyReceived.load(std::memory_order_acquire))
    {
      gatewayRecoveryActive = false;
      continue;
    }

    if (shardConnected)
    {
      if (gatewayRecoveryActive)
      {
        const auto elapsed = std::chrono::steady_clock::now() - gatewayRecoveryStart;
        logging::info(
          "Gateway shard recovered after {}s",
          std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        gatewayRecoveryActive = false;
      }
      continue;
    }

    if (!gatewayRecoveryActive)
    {
      gatewayRecoveryActive = true;
      gatewayRecoveryStart = std::chrono::steady_clock::now();
      logging::warn(
        "Gateway shard disconnected; waiting up to {}s for recovery",
        std::chrono::duration_cast<std::chrono::seconds>(botConfig().gatewayReconnectTimeout)
          .count());
      continue;
    }

    const auto elapsed = std::chrono::steady_clock::now() - gatewayRecoveryStart;
    if (elapsed >= botConfig().gatewayReconnectTimeout)
    {
      restartForGatewayStall = true;
      logging::error(
        "Gateway reconnect stalled for {}s; exiting so the supervisor can restart the bot",
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
      break;
    }
  }

  if (restartForGatewayStall)
  {
    logging::error("Skipping graceful shutdown after gateway stall; forcing process restart");
    std::cerr << std::flush;
    std::_Exit(1);
  }

  logging::info("Shutting down...");

  // Send goodbye message before tearing anything down
  auto respCh = responseChannel();
  if (!restartForGatewayStall && respCh != dpp::snowflake(0))
  {
    bot->message_create(dpp::message()
      .set_channel_id(respCh)
      .set_content(std::format("Time's up, gotta go. Adios! {}", botConfig().botReactionImage)));
    // Give the HTTP request time to dispatch before we tear down
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  musicQueue.shutdown();
  musicQueue.requestSkip();

  {
    std::lock_guard sessionLock(voiceSessionMutex);
    if (voiceSessionThread.joinable())
    {
      voiceSessionThread.join();
    }
  }

  dpp::discord_client* shutdownShard = bot ? bot->get_shard(0) : nullptr;

  if (voiceConnected.exchange(false, std::memory_order_acq_rel))
  {
    {
      std::lock_guard lock(voiceClientMutex);
      activeVoiceClient = nullptr;
    }
    auto guildId = dpp::snowflake(currentGuildId.load(std::memory_order_acquire));
    if (shutdownShard)
    {
      shutdownShard->disconnect_voice(guildId);
    }
  }

  if (shutdownShard)
  {
    shutdownShard->send_close_packet();
  }

  // Join all tracked disconnect threads to guarantee they finish before
  // bot is destroyed — eliminates use-after-free on the cluster pointer.
  {
    std::lock_guard lock(disconnectThreadsMutex);
    for (auto& t : disconnectThreads)
    {
      if (t.thread.joinable())
      {
        t.thread.join();
      }
    }
    disconnectThreads.clear();
  }

  bot->shutdown();

  return restartForGatewayStall ? 1 : 0;
}
