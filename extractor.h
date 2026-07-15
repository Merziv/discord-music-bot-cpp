#pragma once

#include <atomic>
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

/// @param startClient  Rotate starting Innertube player-client (default 0).
[[nodiscard]] std::expected<ExtractedInfo, std::string>
  extractStreamInfo(std::string_view videoUrl, const std::atomic<bool>& cancelFlag, size_t startClient = 0);

/// @note Values of @p maxResults above 25 are silently clamped to 25.
[[nodiscard]] std::vector<std::string>
  searchCandidateUrls(std::string_view query, const std::atomic<bool>& cancelFlag, size_t maxResults = 10);

[[nodiscard]] std::optional<PlaylistInfo> extractPlaylistInfo(std::string_view query);
