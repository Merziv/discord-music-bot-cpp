#include "audio_streamer.h"
#include "config_manager.h"
#include "extractor.h"
#include "queue_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
std::mutex shutdownMutex;
std::condition_variable shutdownCV;

MusicQueueManager musicQueue;

std::unique_ptr<dpp::cluster> bot;

std::atomic<dpp::snowflake> currentGuildId{0};
std::atomic<dpp::discord_client*> currentShard{nullptr};
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

[[nodiscard]] inline const config::BotConfig& botConfig()
{
  return config::getConfig().bot();
}

[[nodiscard]] inline dpp::snowflake botChannelId()
{
  return dpp::snowflake(botConfig().botChannelId);
}

}  // namespace

void signalHandler(int /*signal*/)
{
  shutdownRequested.store(true, std::memory_order_release);
  shutdownCV.notify_one();
}

void streamAudio(dpp::discord_voice_client* voiceClient, const QueueItem& item)
{
  logging::info("Starting streamAudio. Query: {}", item.query);
  musicQueue.setPlaying(true);

  auto trackStart = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(playTimeMutex);
    currentTrackStart = trackStart;
    isCurrentlyPlaying.store(true, std::memory_order_release);
  }

  auto result = extractStreamInfo(item.query);
  if (!result)
  {
    logging::error("Extractor error: {}", result.error());
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
                          .set_content(std::format("❌ Error: {}", result.error())));
    musicQueue.setPlaying(false);
    isCurrentlyPlaying.store(false, std::memory_order_release);
    return;
  }

  const auto& info = *result;
  logging::info("Now playing: '{}'", info.title);
  {
    std::lock_guard lock(playTimeMutex);
    currentTrackTitle = info.title;
  }
  if (bot)
  {
    std::string pres = info.title;
    if (pres.size() > 120)
    {
      pres = pres.substr(0, 117) + "...";
    }
    bot->set_presence(dpp::presence(dpp::ps_online, dpp::at_listening, pres));
  }

  bot->message_create(
    dpp::message()
      .set_channel_id(botChannelId())
      .set_content(std::format(
        "🎵 Now playing: '{}'\n{} {}", info.title, info.webpageUrl, botConfig().botReactionImage)));

  logging::debug(
    "Stream URL (truncated): {}...",
    info.streamUrl.substr(0, std::min(info.streamUrl.size(), size_t{128})));

  try
  {
    audio::PlaybackController controller{
      .shouldStop = [] { return musicQueue.shouldStop(); },
      .isPaused = [] { return musicQueue.isPaused(); },
      .waitWhilePaused = [] { return musicQueue.waitWhilePaused(); },
    };
    audio::AudioStreamer streamer(voiceClient, info.streamUrl, info.title, std::move(controller));
    streamer.start();
  }
  catch (const std::exception& e)
  {
    logging::error("Audio streaming error: {}", e.what());
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
                          .set_content(std::format("❌ Playback error: {}", e.what())));
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

void voiceSessionLoop(dpp::discord_voice_client* voiceClient, const dpp::snowflake guildId)
{
  logging::info("Voice session started for guild {}", static_cast<uint64_t>(guildId));

  voiceSessionStart.store(std::chrono::steady_clock::now(), std::memory_order_release);
  totalPlayTime.store(std::chrono::steady_clock::duration::zero(), std::memory_order_release);

  while (!shutdownRequested.load(std::memory_order_acquire))
  {
    auto item = musicQueue.waitForItem(std::chrono::seconds(30));

    if (item)
    {
      streamAudio(voiceClient, *item);
      continue;
    }

    auto idleTime = std::chrono::steady_clock::now() - musicQueue.lastActivityTime();
    if (idleTime > botConfig().idleTimeout && musicQueue.isIdle())
    {
      logging::info("Idle timeout reached, disconnecting from voice");
      bot->message_create(
        dpp::message()
          .set_channel_id(botChannelId())
          .set_content(
            std::format("👋 Disconnecting due to inactivity {}", botConfig().botReactionImage)));
      break;
    }
  }

  if (auto* shard = currentShard.load(std::memory_order_acquire))
  {
    shard->disconnect_voice(guildId);
  }
  voiceConnected.store(false, std::memory_order_release);
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
                          .set_channel_id(botChannelId())
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
                          .set_channel_id(botChannelId())
                          .set_content("❌ You must be in a voice channel to use this command"));
    return;
  }

  const auto channelId = it->second.channel_id;

  if (auto playlist = extractPlaylistInfo(query))
  {
    if (playlist->videoUrls.empty())
    {
      bot->message_create(dpp::message()
                            .set_channel_id(botChannelId())
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
                            .set_channel_id(botChannelId())
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

      if (auto* shard = event.from)
      {
        currentShard.store(shard, std::memory_order_release);
        currentGuildId.store(msg.guild_id, std::memory_order_release);
        logging::debug("Initiating voice connection");
        shard->connect_voice(msg.guild_id, channelId, false, false, false);
      }

      bot->message_create(
        dpp::message()
          .set_channel_id(botChannelId())
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
        dpp::message().set_channel_id(botChannelId()).set_content("📝 Added to front of queue"));
    }
    else
    {
      musicQueue.enqueue(std::move(item));
      bot->message_create(
        dpp::message()
          .set_channel_id(botChannelId())
          .set_content(std::format("📝 Added to queue (position {})", musicQueue.size())));
    }
  }
  else
  {
    musicQueue.enqueue(std::move(item));

    if (auto* shard = event.from)
    {
      currentShard.store(shard, std::memory_order_release);
      currentGuildId.store(msg.guild_id, std::memory_order_release);
      logging::debug("Initiating voice connection");
      shard->connect_voice(msg.guild_id, channelId, false, false, false);
    }
  }
}

void handleSkipCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("❌ Nothing is playing"));
    return;
  }

  musicQueue.requestSkip();
  bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content("⏭️ Skipping..."));
}

void handleQueueCommand(const dpp::message_create_t& /*event*/, std::string_view args)
{
  const size_t queueSize = musicQueue.size();
  if (queueSize == 0 && !musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("📭 Queue is empty"));
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
        .set_channel_id(botChannelId())
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

  bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content(content));
}

void handleStopCommand(const dpp::message_create_t& /*event*/)
{
  musicQueue.clear();
  musicQueue.setPaused(false);
  musicQueue.requestSkip();

  bot->message_create(dpp::message()
                        .set_channel_id(botChannelId())
                        .set_content("⏹️ Stopped playback and cleared queue"));
}

void handlePauseCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("❌ Nothing is playing"));
    return;
  }

  if (musicQueue.isPaused())
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("⏸️ Already paused"));
    return;
  }

  musicQueue.setPaused(true);
  bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content("⏸️ Paused"));
}

void handleResumeCommand(const dpp::message_create_t& /*event*/)
{
  if (!musicQueue.isPlaying())
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("❌ Nothing is playing"));
    return;
  }

  if (!musicQueue.isPaused())
  {
    bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content("▶️ Not paused"));
    return;
  }

  musicQueue.setPaused(false);
  bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content("▶️ Resumed"));
}

void handleClearCommand(const dpp::message_create_t& /*event*/)
{
  const size_t cleared = musicQueue.size();
  musicQueue.clear();

  if (cleared == 0)
  {
    bot->message_create(
      dpp::message().set_channel_id(botChannelId()).set_content("📭 Queue was already empty"));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
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
                          .set_channel_id(botChannelId())
                          .set_content("❌ Need at least 2 items in queue to shuffle"));
    return;
  }

  musicQueue.shuffle();
  bot->message_create(dpp::message()
                        .set_channel_id(botChannelId())
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

  bot->message_create(dpp::message().set_channel_id(botChannelId()).set_content(content));
}

void handleRemoveCommand(const dpp::message_create_t& /*event*/, std::string_view args)
{
  if (args.empty())
  {
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
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
        dpp::message().set_channel_id(botChannelId()).set_content("❌ Invalid range"));
      return;
    }

    const size_t removed = musicQueue.removeRange(startPos, endPos);
    if (removed == 0)
    {
      bot->message_create(dpp::message()
                            .set_channel_id(botChannelId())
                            .set_content("❌ Invalid range or queue is empty"));
    }
    else
    {
      bot->message_create(dpp::message()
                            .set_channel_id(botChannelId())
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
        dpp::message().set_channel_id(botChannelId()).set_content("❌ Invalid position"));
      return;
    }

    if (musicQueue.remove(startPos))
    {
      bot->message_create(dpp::message()
                            .set_channel_id(botChannelId())
                            .set_content(std::format("🗑️ Removed item at position {}", startPos)));
    }
    else
    {
      bot->message_create(dpp::message()
                            .set_channel_id(botChannelId())
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
        .set_channel_id(botChannelId())
        .set_content(std::format("Usage: `{}move <from> <to>`", botConfig().commandPrefix)));
    return;
  }

  const auto spacePos = args.find(' ');
  if (spacePos == std::string_view::npos)
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(botChannelId())
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
      dpp::message().set_channel_id(botChannelId()).set_content("❌ Invalid positions"));
    return;
  }

  if (musicQueue.move(fromPos, toPos))
  {
    bot->message_create(
      dpp::message()
        .set_channel_id(botChannelId())
        .set_content(std::format("↕️ Moved item from position {} to {}", fromPos, toPos)));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
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
      dpp::message().set_channel_id(botChannelId()).set_content("📭 Nothing is playing or queued"));
    return;
  }

  if (playing)
  {
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
                          .set_content(std::format(
                            "🎧 Now playing: '{}' • elapsed: {}", title, formatDuration(elapsed))));
  }
  else
  {
    bot->message_create(dpp::message()
                          .set_channel_id(botChannelId())
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
  logging::info("Config loaded from: {}", configPath);

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  botStartTime = std::chrono::steady_clock::now();

  bot = std::make_unique<dpp::cluster>(
    botConfig().botToken, dpp::i_all_intents | dpp::i_message_content);

  bot->on_log(dpp::utility::cout_logger());

  bot->on_ready([](const dpp::ready_t&) {
    logging::info("{}", botConfig().startupMessage);
    bot->set_presence(
      dpp::presence(dpp::ps_online, dpp::at_game, std::string(botConfig().statusPlayingGame)));
  });

  bot->on_message_create([](const dpp::message_create_t& event) {
    const auto& msg = event.msg;

    if (msg.channel_id != botChannelId())
    {
      return;
    }

    if (
      msg.author.id == bot->me.id || msg.content.empty()
      || !msg.content.starts_with(botConfig().commandPrefix))
    {
      return;
    }

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
      voiceConnected.store(true, std::memory_order_release);
      auto guildId = currentGuildId.load(std::memory_order_acquire);

      if (voiceSessionThread.joinable())
      {
        voiceSessionThread.join();
      }
      voiceSessionThread = std::jthread(voiceSessionLoop, vc, guildId);
    }
  });

  bot->start(dpp::st_return != 0U);

  {
    std::unique_lock lock(shutdownMutex);
    shutdownCV.wait(lock, [] { return shutdownRequested.load(std::memory_order_acquire); });
  }

  logging::info("Shutting down...");

  musicQueue.shutdown();
  musicQueue.requestSkip();

  if (voiceSessionThread.joinable())
  {
    voiceSessionThread.join();
  }

  bot->shutdown();

  return 0;
}
