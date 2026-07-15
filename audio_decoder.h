#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

class FFmpegAudioDecoder
{
public:
  using SamplesCallback = std::function<bool(std::span<const int16_t>)>;

  [[nodiscard]] std::expected<void, std::string>
    decode(std::string_view streamUrl, const std::stop_token& stopToken, SamplesCallback onSamples) const;
};