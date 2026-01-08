#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace config {

struct BotConfig
{
  std::string botToken;
  std::string startupMessage;
  std::string statusPlayingGame;
  std::string commandPrefix;
  uint64_t botChannelId{0};
  std::string botReactionImage;
  std::chrono::minutes idleTimeout{5};
};

struct AppConfig
{
  BotConfig bot;
};

class ConfigManager
{
  AppConfig _config;
  bool _loaded{false};

public:
  ConfigManager() = default;

  [[nodiscard]] static std::expected<ConfigManager, std::string>
    loadFromFile(std::string_view filePath);

  [[nodiscard]] const AppConfig& config() const noexcept
  {
    return _config;
  }

  [[nodiscard]] const BotConfig& bot() const noexcept
  {
    return _config.bot;
  }

  [[nodiscard]] bool isLoaded() const noexcept
  {
    return _loaded;
  }
};

ConfigManager& getConfig();
void setGlobalConfig(ConfigManager manager);

}  // namespace config
