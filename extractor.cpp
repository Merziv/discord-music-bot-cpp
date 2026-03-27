#include "extractor.h"

#include "audio_streamer.h"

#include <array>
#include <cstdio>
#include <format>
#include <rapidjson/document.h>

namespace logging = audio::logging;

namespace {

[[nodiscard]] std::expected<std::string, std::string> runCommand(std::string_view cmd)
{
  std::string fullCmd = std::format("{} 2>&1", cmd);

  FILE* pipe = popen(fullCmd.c_str(), "r");
  if (pipe == nullptr)
  {
    return std::unexpected(std::format("Failed to spawn: {}", cmd));
  }

  std::string output;
  output.reserve(16384);
  std::array<char, 8192> readBuffer{};

  while (true)
  {
    const size_t n = std::fread(readBuffer.data(), 1, readBuffer.size(), pipe);
    if (n > 0)
    {
      output.append(readBuffer.data(), n);
    }
    if (n < readBuffer.size())
    {
      if (std::feof(pipe) != 0 || std::ferror(pipe) != 0)
      {
        break;
      }
    }
  }

  const int code = pclose(pipe);
  if (code != 0)
  {
    return std::unexpected(std::format("Command exited with code {}. Output:\n{}", code, output));
  }
  return output;
}

[[nodiscard]] std::string shellQuote(std::string_view str)
{
  std::string quotedOutput;
  quotedOutput.reserve(str.size() + 10);
  quotedOutput.push_back('\'');
  for (char c : str)
  {
    if (c == '\'')
    {
      quotedOutput += "'\\''";
    }
    else if (c == '\0')
    {
      continue;
    }
    else
    {
      quotedOutput.push_back(c);
    }
  }
  quotedOutput.push_back('\'');
  return quotedOutput;
}

namespace json {

[[nodiscard]] std::optional<std::string> getString(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsString())
  {
    return std::string(obj[key].GetString(), obj[key].GetStringLength());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<double> getDouble(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsNumber())
  {
    return obj[key].GetDouble();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<bool> getBool(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsBool())
  {
    return obj[key].GetBool();
  }
  return std::nullopt;
}

[[nodiscard]] const rapidjson::Value* getArray(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsArray())
  {
    return &obj[key];
  }
  return nullptr;
}

[[nodiscard]] std::optional<rapidjson::Document> tryParseJson(std::string_view blob)
{
  for (size_t pos = 0; (pos = blob.find('{', pos)) != std::string_view::npos; ++pos)
  {
    rapidjson::Document doc;
    doc.Parse(blob.data() + pos, blob.size() - pos);
    if (!doc.HasParseError() && doc.IsObject())
    {
      return doc;
    }
  }
  return std::nullopt;
}

}  // namespace json

[[nodiscard]] std::optional<std::string> chooseBestAudioUrl(const rapidjson::Value& formats)
{
  if (!formats.IsArray())
  {
    return std::nullopt;
  }

  const rapidjson::Value* best = nullptr;
  double bestBitrate = -1.0;

  for (const auto& f : formats.GetArray())
  {
    if (!f.IsObject())
    {
      continue;
    }

    const auto vcodec = json::getString(f, "vcodec").value_or("");
    const auto acodec = json::getString(f, "acodec").value_or("");

    if (acodec == "none" || vcodec != "none")
    {
      continue;
    }

    const double audioBitrate = json::getDouble(f, "abr").value_or(-1.0);
    if (audioBitrate > bestBitrate)
    {
      bestBitrate = audioBitrate;
      best = &f;
    }
  }

  if (best == nullptr)
  {
    for (const auto& f : formats.GetArray())
    {
      if (!f.IsObject())
      {
        continue;
      }

      const auto acodec = json::getString(f, "acodec").value_or("");
      if (acodec == "none")
      {
        continue;
      }

      const double audioBitrate = json::getDouble(f, "abr").value_or(-1.0);
      if (audioBitrate > bestBitrate)
      {
        bestBitrate = audioBitrate;
        best = &f;
      }
    }
  }

  if (best != nullptr)
  {
    return json::getString(*best, "url");
  }
  return std::nullopt;
}

[[nodiscard]] const rapidjson::Value* findLeafEntry(const rapidjson::Value& root)
{
  if (const auto* entries = json::getArray(root, "entries"))
  {
    for (const auto& e : entries->GetArray())
    {
      if (!e.IsNull())
      {
        return &e;
      }
    }
  }
  return nullptr;
}

}  // namespace

std::expected<ExtractedInfo, std::string> extractStreamInfo(std::string_view query)
{
  struct Attempt
  {
    const char* label;
    const char* extractorArgs;
  };

  static constexpr std::array attempts = {
    Attempt{.label = "android", .extractorArgs = "youtube:player-client=android"},
    Attempt{.label = "tv", .extractorArgs = "youtube:player-client=tv"},
    Attempt{.label = "ios", .extractorArgs = "youtube:player-client=ios"},
    Attempt{.label = "web", .extractorArgs = "youtube:player-client=web"},
    Attempt{.label = "none", .extractorArgs = ""},
  };

  auto buildCmd = [&](const Attempt& a, bool /*allowPlaylist*/ = false) {
    std::string cmd = "yt-dlp ";
    if (a.extractorArgs[0] != '\0')
    {
      cmd += std::format("--extractor-args {} ", a.extractorArgs);
    }
    cmd += "-f bestaudio/best --dump-single-json ";
    cmd += "--no-playlist ";
    cmd +=
      "--ignore-config --no-check-certificate "
      "--default-search auto --quiet --no-warnings "
      "--retries 5 --source-address 0.0.0.0 ";
    cmd += shellQuote(query);
    return cmd;
  };

  auto tryBuildFromJson = [](const rapidjson::Document& root) -> std::optional<ExtractedInfo> {
    if (const auto* entries = json::getArray(root, "entries"))
    {
      bool anyNonNull = false;
      for (const auto& e : entries->GetArray())
      {
        if (!e.IsNull())
        {
          anyNonNull = true;
          break;
        }
      }
      if (!anyNonNull)
      {
        return std::nullopt;
      }
    }

    const auto* leafPtr = findLeafEntry(root);
    const auto& leaf = (leafPtr != nullptr) ? *leafPtr : root;

    ExtractedInfo info;
    info.title = json::getString(leaf, "title").value_or("");
    info.webpageUrl = json::getString(leaf, "webpage_url")
                        .or_else([&] { return json::getString(leaf, "original_url"); })
                        .value_or("");
    info.isLive = json::getBool(leaf, "is_live").value_or(false);

    if (auto durationSeconds = json::getDouble(leaf, "duration"))
    {
      info.durationSec = static_cast<int64_t>(*durationSeconds);
    }

    if (auto url = json::getString(leaf, "url"); url && !url->empty())
    {
      info.streamUrl = *url;
      return info;
    }

    if (const auto* reqFormats = json::getArray(leaf, "requested_formats"))
    {
      for (const auto& f : reqFormats->GetArray())
      {
        if (!f.IsObject())
        {
          continue;
        }
        const auto acodec = json::getString(f, "acodec").value_or("");
        const auto vcodec = json::getString(f, "vcodec").value_or("");
        if (acodec != "none" && vcodec == "none")
        {
          if (auto url = json::getString(f, "url"); url && !url->empty())
          {
            info.streamUrl = *url;
            return info;
          }
        }
      }
      for (const auto& f : reqFormats->GetArray())
      {
        if (!f.IsObject())
        {
          continue;
        }
        const auto acodec = json::getString(f, "acodec").value_or("");
        if (acodec != "none")
        {
          if (auto url = json::getString(f, "url"); url && !url->empty())
          {
            info.streamUrl = *url;
            return info;
          }
        }
      }
    }

    if (const auto* formats = json::getArray(leaf, "formats"))
    {
      if (auto chosen = chooseBestAudioUrl(*formats))
      {
        info.streamUrl = *chosen;
        return info;
      }
    }

    return std::nullopt;
  };

  for (const auto& a : attempts)
  {
    const std::string cmd = buildCmd(a);
    logging::debug("Running yt-dlp ({}): {}", a.label, cmd);

    auto result = runCommand(cmd);
    std::string_view blob;
    std::string errorBlob;

    if (result)
    {
      blob = *result;
      logging::debug("yt-dlp ({}) success, output size={}", a.label, blob.size());
    }
    else
    {
      errorBlob = std::move(result.error());
      blob = errorBlob;
      logging::warn("yt-dlp ({}) failed, trying to salvage JSON. Size={}", a.label, blob.size());
    }

    auto parsed = json::tryParseJson(blob);
    if (!parsed)
    {
      logging::warn("yt-dlp ({}) produced no parseable JSON", a.label);
      continue;
    }

    if (auto maybe = tryBuildFromJson(*parsed))
    {
      logging::info("Selected stream via '{}'. Title: {}", a.label, maybe->title);
      return *maybe;
    }

    logging::warn("yt-dlp ({}) JSON parsed but no playable URL", a.label);
  }

  return std::unexpected("No playable audio URL found after all extractor attempts");
}

std::optional<PlaylistInfo> extractPlaylistInfo(std::string_view query)
{
  bool isPlaylistUrl = (query.find("list=") != std::string_view::npos)
                       || (query.find("/playlist") != std::string_view::npos);

  if (!isPlaylistUrl)
  {
    return std::nullopt;
  }

  logging::info("Detected playlist URL, extracting entries...");

  std::string cmd = std::format(
    "yt-dlp --flat-playlist --dump-single-json "
    "--ignore-config --no-check-certificate "
    "--quiet --no-warnings "
    "--retries 3 --source-address 0.0.0.0 {}",
    shellQuote(query));

  logging::debug("Running yt-dlp playlist extraction: {}", cmd);

  auto result = runCommand(cmd);
  if (!result)
  {
    logging::warn("Playlist extraction failed: {}", result.error());
    return std::nullopt;
  }

  auto parsed = json::tryParseJson(*result);
  if (!parsed)
  {
    logging::warn("Failed to parse playlist JSON");
    return std::nullopt;
  }

  const auto& root = *parsed;
  PlaylistInfo info;

  info.playlistTitle = json::getString(root, "title").value_or("Unknown Playlist");

  const auto* entries = json::getArray(root, "entries");
  if (entries == nullptr || entries->Empty())
  {
    logging::warn("Playlist has no entries");
    return std::nullopt;
  }

  info.videoUrls.reserve(entries->Size());

  for (const auto& entry : entries->GetArray())
  {
    if (entry.IsNull())
    {
      continue;
    }

    if (auto url = json::getString(entry, "url"); url && !url->empty())
    {
      if (url->find("http") == std::string::npos)
      {
        info.videoUrls.push_back(std::format("https://www.youtube.com/watch?v={}", *url));
      }
      else
      {
        info.videoUrls.push_back(*url);
      }
    }
    else if (auto id = json::getString(entry, "id"); id && !id->empty())
    {
      info.videoUrls.push_back(std::format("https://www.youtube.com/watch?v={}", *id));
    }
  }

  logging::info(
    "Extracted {} videos from playlist '{}'", info.videoUrls.size(), info.playlistTitle);

  return info;
}
