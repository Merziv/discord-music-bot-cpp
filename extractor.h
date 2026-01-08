#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ExtractedInfo
{
  std::string streamUrl;
  std::string title;
  std::string webpageUrl;
  bool isLive{false};
  std::optional<int64_t> durationSec;
};

struct PlaylistInfo
{
  std::string playlistTitle;
  std::vector<std::string> videoUrls;
};

[[nodiscard]] std::expected<ExtractedInfo, std::string> extractStreamInfo(std::string_view query);

[[nodiscard]] std::optional<PlaylistInfo> extractPlaylistInfo(std::string_view query);
