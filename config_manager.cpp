#include "config_manager.h"

#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <sstream>
#include <string>

namespace config {

namespace {

std::optional<ConfigManager> g_globalConfig;

[[nodiscard]] std::optional<std::string> getString(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsString())
  {
    return std::string(obj[key].GetString(), obj[key].GetStringLength());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<uint64_t> getUint64(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsUint64())
  {
    return obj[key].GetUint64();
  }
  if (obj.HasMember(key) && obj[key].IsInt64())
  {
    return static_cast<uint64_t>(obj[key].GetInt64());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<int> getInt(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsInt())
  {
    return obj[key].GetInt();
  }
  return std::nullopt;
}

[[nodiscard]] const rapidjson::Value* getObject(const rapidjson::Value& obj, const char* key)
{
  if (obj.HasMember(key) && obj[key].IsObject())
  {
    return &obj[key];
  }
  return nullptr;
}

}  // namespace

std::expected<ConfigManager, std::string> ConfigManager::loadFromFile(std::string_view filePath)
{
  std::ifstream file{std::string(filePath)};
  if (!file.is_open())
  {
    return std::unexpected(std::format("Failed to open config file: {}", filePath));
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  rapidjson::Document doc;
  doc.Parse(content.c_str());

  if (doc.HasParseError())
  {
    return std::unexpected(std::format(
      "JSON parse error at offset {}: {}",
      doc.GetErrorOffset(),
      rapidjson::GetParseError_En(doc.GetParseError())));
  }

  if (!doc.IsObject())
  {
    return std::unexpected("Config file must contain a JSON object");
  }

  ConfigManager manager;
  auto& cfg = manager._config;

  if (const auto* botSection = getObject(doc, "bot"))
  {
    if (auto val = getString(*botSection, "token"))
    {
      cfg.bot.botToken = *val;
    }
    else
    {
      return std::unexpected("Missing required field: bot.token");
    }

    if (auto val = getString(*botSection, "startup_message"))
    {
      cfg.bot.startupMessage = *val;
    }
    else
    {
      cfg.bot.startupMessage = "Bot is now online!";
    }

    if (auto val = getString(*botSection, "status_playing_game"))
    {
      cfg.bot.statusPlayingGame = *val;
    }
    else
    {
      cfg.bot.statusPlayingGame = "Music";
    }

    if (auto val = getString(*botSection, "command_prefix"))
    {
      cfg.bot.commandPrefix = *val;
    }
    else
    {
      cfg.bot.commandPrefix = "!";
    }

    if (auto val = getUint64(*botSection, "channel_id"))
    {
      cfg.bot.botChannelId = *val;
    }
    else
    {
      return std::unexpected("Missing required field: bot.channel_id");
    }

    if (auto val = getString(*botSection, "reaction_image"))
    {
      cfg.bot.botReactionImage = *val;
    }
    else
    {
      cfg.bot.botReactionImage = "";
    }

    if (auto val = getInt(*botSection, "idle_timeout_minutes"))
    {
      cfg.bot.idleTimeout = std::chrono::minutes(*val);
    }
    else
    {
      cfg.bot.idleTimeout = std::chrono::minutes(5);
    }
  }
  else
  {
    return std::unexpected("Missing required section: bot");
  }

  manager._loaded = true;
  return manager;
}

ConfigManager& getConfig()
{
  if (!g_globalConfig.has_value())
  {
    throw std::runtime_error("Config not loaded. Call setGlobalConfig() first.");
  }
  return *g_globalConfig;
}

void setGlobalConfig(ConfigManager manager)
{
  g_globalConfig = std::move(manager);
}

}  // namespace config
