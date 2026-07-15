#include "extractor.h"

#include "audio_streamer.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace logging = audio::logging;

namespace {

constexpr long HTTP_TIMEOUT_SECONDS = 12L;
constexpr long HTTP_CONNECT_TIMEOUT_SECONDS = 8L;
constexpr size_t MAX_RESULTS_LIMIT = 25;

constexpr std::string_view YOUTUBE_WATCH_PREFIX = "https://www.youtube.com/watch?v=";

constexpr const char* WEB_API_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
constexpr const char* ANDROID_API_KEY = "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w";

constexpr const char* WEB_CLIENT_VERSION = "2.20250213.01.00";
constexpr const char* ANDROID_VR_CLIENT_VERSION = "1.65.10";
constexpr const char* ANDROID_CLIENT_VERSION = "19.44.38";
constexpr const char* TV_CLIENT_VERSION = "7.20250212.19.00";
constexpr const char* IOS_CLIENT_VERSION = "19.45.4";

constexpr const char* WEB_USER_AGENT =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr const char* ANDROID_VR_USER_AGENT =
  "com.google.android.apps.youtube.vr.oculus/1.65.10 "
  "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
constexpr const char* ANDROID_USER_AGENT =
  "com.google.android.youtube/19.44.38 (Linux; U; Android 14)";
constexpr const char* IOS_USER_AGENT =
  "com.google.ios.youtube/19.45.4 (iPhone16,2; U; CPU iOS 17_0 like Mac OS X)";
constexpr const char* TV_USER_AGENT =
  "Mozilla/5.0 (SMART-TV; Linux; Tizen 7.0) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

struct HttpResponse
{
  long statusCode{0};
  std::string body;
};

struct ClientProfile
{
  const char* label;
  const char* apiKey;
  const char* clientName;
  int clientHeaderName;
  const char* clientVersion;
  const char* userAgent;
  const char* gl;
  const char* deviceMake;
  const char* deviceModel;
  const char* osName;
  const char* osVersion;
  int androidSdkVersion;
  bool useLiveClientVersion;
  bool includeVisitorDataInClient;
};

struct WatchPageContext
{
  std::string webClientVersion;
  std::string visitorData;
  int signatureTimestamp{0};
};

static constexpr std::array kPlayerClients = {
  ClientProfile{
    .label = "android_vr",
    .apiKey = ANDROID_API_KEY,
    .clientName = "ANDROID_VR",
    .clientHeaderName = 28,
    .clientVersion = ANDROID_VR_CLIENT_VERSION,
    .userAgent = ANDROID_VR_USER_AGENT,
    .gl = nullptr,
    .deviceMake = "Oculus",
    .deviceModel = "Quest 3",
    .osName = "Android",
    .osVersion = "12L",
    .androidSdkVersion = 32,
    .useLiveClientVersion = false,
    .includeVisitorDataInClient = false,
  },
  ClientProfile{
    .label = "web",
    .apiKey = WEB_API_KEY,
    .clientName = "WEB",
    .clientHeaderName = 1,
    .clientVersion = WEB_CLIENT_VERSION,
    .userAgent = WEB_USER_AGENT,
    .gl = "US",
    .deviceMake = nullptr,
    .deviceModel = nullptr,
    .osName = nullptr,
    .osVersion = nullptr,
    .androidSdkVersion = 0,
    .useLiveClientVersion = true,
    .includeVisitorDataInClient = true,
  },
  ClientProfile{
    .label = "android",
    .apiKey = ANDROID_API_KEY,
    .clientName = "ANDROID",
    .clientHeaderName = 3,
    .clientVersion = ANDROID_CLIENT_VERSION,
    .userAgent = ANDROID_USER_AGENT,
    .gl = nullptr,
    .deviceMake = nullptr,
    .deviceModel = nullptr,
    .osName = nullptr,
    .osVersion = nullptr,
    .androidSdkVersion = 34,
    .useLiveClientVersion = false,
    .includeVisitorDataInClient = false,
  },
  ClientProfile{
    .label = "tv",
    .apiKey = WEB_API_KEY,
    .clientName = "TVHTML5_SIMPLY_EMBEDDED_PLAYER",
    .clientHeaderName = 7,
    .clientVersion = TV_CLIENT_VERSION,
    .userAgent = TV_USER_AGENT,
    .gl = nullptr,
    .deviceMake = nullptr,
    .deviceModel = nullptr,
    .osName = nullptr,
    .osVersion = nullptr,
    .androidSdkVersion = 0,
    .useLiveClientVersion = false,
    .includeVisitorDataInClient = false,
  },
  ClientProfile{
    .label = "ios",
    .apiKey = WEB_API_KEY,
    .clientName = "IOS",
    .clientHeaderName = 5,
    .clientVersion = IOS_CLIENT_VERSION,
    .userAgent = IOS_USER_AGENT,
    .gl = nullptr,
    .deviceMake = nullptr,
    .deviceModel = nullptr,
    .osName = nullptr,
    .osVersion = nullptr,
    .androidSdkVersion = 0,
    .useLiveClientVersion = false,
    .includeVisitorDataInClient = false,
  },
};

void ensureCurlGlobalInit()
{
  static std::once_flag once;
  std::call_once(once, [] {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
    std::atexit([] { curl_global_cleanup(); });
  });
}

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  if (ptr == nullptr || userdata == nullptr)
  {
    return 0;
  }

  const size_t bytes = size * nmemb;
  auto* output = static_cast<std::string*>(userdata);
  output->append(ptr, bytes);
  return bytes;
}

size_t curlDiscardCallback(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/)
{
  return size * nmemb;
}

[[nodiscard]] std::expected<HttpResponse, std::string> httpRequest(
  std::string_view url,
  std::optional<std::string_view> postBody,
  const std::vector<std::string>& headers,
  std::string_view userAgent)
{
  ensureCurlGlobalInit();

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl)
  {
    return std::unexpected("Failed to initialize curl");
  }

  HttpResponse response;
  const std::string urlStorage(url);
  const std::string userAgentStorage(userAgent);

  curl_easy_setopt(curl.get(), CURLOPT_URL, urlStorage.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_SECONDS);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, HTTP_TIMEOUT_SECONDS);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgentStorage.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlWriteCallback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);

  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(nullptr, curl_slist_free_all);
  for (const auto& header : headers)
  {
    curl_slist* appended = curl_slist_append(headerList.get(), header.c_str());
    if (appended == nullptr)
    {
      return std::unexpected("Failed to append curl header");
    }
    headerList.release();
    headerList.reset(appended);
  }

  if (headerList)
  {
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headerList.get());
  }

  if (postBody)
  {
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, postBody->data());
    curl_easy_setopt(
      curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
  }

  const CURLcode performResult = curl_easy_perform(curl.get());
  if (performResult != CURLE_OK)
  {
    return std::unexpected(std::format("HTTP request failed: {}", curl_easy_strerror(performResult)));
  }

  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.statusCode);
  return response;
}

[[nodiscard]] std::expected<long, std::string> probeHttpUrlStatus(
  std::string_view url,
  std::string_view userAgent)
{
  ensureCurlGlobalInit();

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl)
  {
    return std::unexpected("Failed to initialize curl");
  }

  long statusCode = 0;
  const std::string urlStorage(url);
  const std::string userAgentStorage(userAgent);

  curl_easy_setopt(curl.get(), CURLOPT_URL, urlStorage.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_SECONDS);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, HTTP_TIMEOUT_SECONDS);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgentStorage.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlDiscardCallback);
  curl_easy_setopt(curl.get(), CURLOPT_RANGE, "0-15");

  const CURLcode performResult = curl_easy_perform(curl.get());
  if (performResult != CURLE_OK)
  {
    return std::unexpected(std::format("HTTP probe failed: {}", curl_easy_strerror(performResult)));
  }

  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);
  return statusCode;
}

[[nodiscard]] std::optional<std::string> jsonString(const rapidjson::Value& object, const char* key)
{
  if (object.IsObject() && object.HasMember(key) && object[key].IsString())
  {
    return std::string(object[key].GetString(), object[key].GetStringLength());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<bool> jsonBool(const rapidjson::Value& object, const char* key)
{
  if (object.IsObject() && object.HasMember(key) && object[key].IsBool())
  {
    return object[key].GetBool();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<int64_t> parseInt64(std::string_view input)
{
  int64_t value = 0;
  const auto* begin = input.data();
  const auto* end = input.data() + input.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end)
  {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<int64_t> jsonInt64(const rapidjson::Value& object, const char* key)
{
  if (!object.IsObject() || !object.HasMember(key))
  {
    return std::nullopt;
  }

  const auto& value = object[key];
  if (value.IsInt64())
  {
    return value.GetInt64();
  }
  if (value.IsUint64())
  {
    const uint64_t v = value.GetUint64();
    if (v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
      return static_cast<int64_t>(v);
    }
    return std::nullopt;
  }
  if (value.IsString())
  {
    return parseInt64(std::string_view(value.GetString(), value.GetStringLength()));
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> extractQuotedValue(
  std::string_view input,
  std::string_view marker)
{
  const size_t markerPos = input.find(marker);
  if (markerPos == std::string_view::npos)
  {
    return std::nullopt;
  }

  const size_t valueStart = markerPos + marker.size();
  const size_t valueEnd = input.find('"', valueStart);
  if (valueEnd == std::string_view::npos || valueEnd <= valueStart)
  {
    return std::nullopt;
  }

  return std::string(input.substr(valueStart, valueEnd - valueStart));
}

[[nodiscard]] std::optional<int> extractPositiveInt(
  std::string_view input,
  std::string_view marker)
{
  const size_t markerPos = input.find(marker);
  if (markerPos == std::string_view::npos)
  {
    return std::nullopt;
  }

  const size_t valueStart = markerPos + marker.size();
  size_t valueEnd = valueStart;
  while (valueEnd < input.size() && input[valueEnd] >= '0' && input[valueEnd] <= '9')
  {
    ++valueEnd;
  }

  if (valueEnd == valueStart)
  {
    return std::nullopt;
  }

  auto parsed = parseInt64(input.substr(valueStart, valueEnd - valueStart));
  if (!parsed || *parsed <= 0 || *parsed > static_cast<int64_t>(std::numeric_limits<int>::max()))
  {
    return std::nullopt;
  }

  return static_cast<int>(*parsed);
}

[[nodiscard]] bool isValidVideoId(std::string_view id)
{
  if (id.size() != 11)
  {
    return false;
  }

  for (const char c : id)
  {
    const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!isAlphaNum && c != '_' && c != '-')
    {
      return false;
    }
  }

  return true;
}

[[nodiscard]] std::string percentDecode(std::string_view input)
{
  std::string out;
  out.reserve(input.size());

  for (size_t i = 0; i < input.size(); ++i)
  {
    const char c = input[i];
    if (c == '%' && (i + 2) < input.size())
    {
      auto hexValue = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
        {
          return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f')
        {
          return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F')
        {
          return 10 + (ch - 'A');
        }
        return -1;
      };

      const int hi = hexValue(input[i + 1]);
      const int lo = hexValue(input[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        const auto byteValue = static_cast<unsigned char>((hi << 4) | lo);
        out.push_back(static_cast<char>(byteValue));
        i += 2;
        continue;
      }
    }

    if (c == '+')
    {
      out.push_back(' ');
    }
    else
    {
      out.push_back(c);
    }
  }

  return out;
}

[[nodiscard]] std::optional<std::string> queryParam(std::string_view url, std::string_view key)
{
  const size_t qPos = url.find('?');
  if (qPos == std::string_view::npos)
  {
    return std::nullopt;
  }

  const size_t fragPos = url.find('#', qPos + 1);
  std::string_view query = url.substr(
    qPos + 1,
    (fragPos == std::string_view::npos) ? std::string_view::npos : (fragPos - qPos - 1));

  size_t pos = 0;
  while (pos < query.size())
  {
    const size_t amp = query.find('&', pos);
    const std::string_view token = query.substr(
      pos,
      (amp == std::string_view::npos) ? std::string_view::npos : (amp - pos));

    const size_t eq = token.find('=');
    const std::string_view tokenKey = token.substr(0, eq);
    if (tokenKey == key)
    {
      if (eq == std::string_view::npos)
      {
        return std::string{};
      }
      return percentDecode(token.substr(eq + 1));
    }

    if (amp == std::string_view::npos)
    {
      break;
    }
    pos = amp + 1;
  }

  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> pathSegmentAfter(
  std::string_view input,
  std::string_view marker)
{
  const size_t pos = input.find(marker);
  if (pos == std::string_view::npos)
  {
    return std::nullopt;
  }

  std::string_view value = input.substr(pos + marker.size());
  const size_t end = value.find_first_of("/?&#");
  if (end != std::string_view::npos)
  {
    value = value.substr(0, end);
  }

  if (value.empty())
  {
    return std::nullopt;
  }

  return std::string(value);
}

[[nodiscard]] bool isLikelyUrl(std::string_view query)
{
  return query.starts_with("http://") || query.starts_with("https://")
         || query.starts_with("www.youtube.com/") || query.starts_with("youtube.com/")
         || query.starts_with("www.youtu.be/") || query.starts_with("youtu.be/");
}

[[nodiscard]] std::string normalizeYoutubeUrl(std::string_view input)
{
  if (input.starts_with("http://") || input.starts_with("https://"))
  {
    return std::string(input);
  }

  return std::format("https://{}", input);
}

[[nodiscard]] std::optional<std::string> extractVideoId(std::string_view query)
{
  if (isValidVideoId(query))
  {
    return std::string(query);
  }

  if (auto v = queryParam(query, "v"); v && isValidVideoId(*v))
  {
    return *v;
  }

  if (auto id = pathSegmentAfter(query, "youtu.be/"); id && isValidVideoId(*id))
  {
    return *id;
  }

  if (auto id = pathSegmentAfter(query, "/shorts/"); id && isValidVideoId(*id))
  {
    return *id;
  }

  if (auto id = pathSegmentAfter(query, "/live/"); id && isValidVideoId(*id))
  {
    return *id;
  }

  return std::nullopt;
}

[[nodiscard]] std::expected<WatchPageContext, std::string> fetchWatchPageContext(
  std::string_view videoId)
{
  const std::string watchUrl = std::format(
    "https://www.youtube.com/watch?v={}&bpctr=9999999999&has_verified=1",
    videoId);

  const auto response = httpRequest(watchUrl, std::nullopt, {}, WEB_USER_AGENT);
  if (!response)
  {
    return std::unexpected(response.error());
  }

  if (response->statusCode < 200 || response->statusCode >= 300)
  {
    return std::unexpected(std::format("watch page returned HTTP {}", response->statusCode));
  }

  auto clientVersion = extractQuotedValue(response->body, R"("INNERTUBE_CLIENT_VERSION":")");
  auto visitorData = extractQuotedValue(response->body, R"("visitorData":")");
  auto signatureTimestamp = extractPositiveInt(response->body, R"("STS":)");

  if (!clientVersion || !visitorData || !signatureTimestamp)
  {
    return std::unexpected("watch page missing live WEB player context");
  }

  return WatchPageContext{
    .webClientVersion = std::move(*clientVersion),
    .visitorData = std::move(*visitorData),
    .signatureTimestamp = *signatureTimestamp,
  };
}

[[nodiscard]] std::string buildPlayerBody(
  const ClientProfile& profile,
  std::string_view videoId,
  const WatchPageContext* watchContext = nullptr)
{
  rapidjson::Document body;
  body.SetObject();

  auto& allocator = body.GetAllocator();
  const bool hasWatchContext = watchContext != nullptr;
  const std::string_view clientVersion = profile.useLiveClientVersion && hasWatchContext
                                          ? std::string_view(watchContext->webClientVersion)
                                          : std::string_view(profile.clientVersion);

  rapidjson::Value context(rapidjson::kObjectType);
  rapidjson::Value client(rapidjson::kObjectType);
  client.AddMember("clientName", rapidjson::Value(profile.clientName, allocator), allocator);
  client.AddMember(
    "clientVersion",
    rapidjson::Value(
      clientVersion.data(),
      static_cast<rapidjson::SizeType>(clientVersion.size()),
      allocator),
    allocator);
  client.AddMember("hl", "en", allocator);
  client.AddMember("timeZone", "UTC", allocator);
  client.AddMember("utcOffsetMinutes", 0, allocator);
  if (profile.gl != nullptr)
  {
    client.AddMember("gl", rapidjson::Value(profile.gl, allocator), allocator);
  }
  if (profile.deviceMake != nullptr)
  {
    client.AddMember("deviceMake", rapidjson::Value(profile.deviceMake, allocator), allocator);
  }
  if (profile.deviceModel != nullptr)
  {
    client.AddMember("deviceModel", rapidjson::Value(profile.deviceModel, allocator), allocator);
  }
  if (profile.androidSdkVersion > 0)
  {
    client.AddMember("androidSdkVersion", profile.androidSdkVersion, allocator);
  }
  if (profile.osName != nullptr)
  {
    client.AddMember("osName", rapidjson::Value(profile.osName, allocator), allocator);
  }
  if (profile.osVersion != nullptr)
  {
    client.AddMember("osVersion", rapidjson::Value(profile.osVersion, allocator), allocator);
  }
  if (profile.includeVisitorDataInClient && hasWatchContext)
  {
    client.AddMember(
      "visitorData",
      rapidjson::Value(
        watchContext->visitorData.c_str(),
        static_cast<rapidjson::SizeType>(watchContext->visitorData.size()),
        allocator),
      allocator);
  }

  context.AddMember("client", client, allocator);
  body.AddMember("context", context, allocator);

  body.AddMember("videoId", rapidjson::Value(videoId.data(), static_cast<rapidjson::SizeType>(videoId.size()), allocator), allocator);
  body.AddMember("contentCheckOk", true, allocator);
  body.AddMember("racyCheckOk", true, allocator);

  if (hasWatchContext)
  {
    rapidjson::Value playbackContext(rapidjson::kObjectType);
    rapidjson::Value contentPlaybackContext(rapidjson::kObjectType);
    contentPlaybackContext.AddMember("html5Preference", "HTML5_PREF_WANTS", allocator);
    contentPlaybackContext.AddMember(
      "signatureTimestamp", watchContext->signatureTimestamp, allocator);
    playbackContext.AddMember("contentPlaybackContext", contentPlaybackContext, allocator);
    body.AddMember("playbackContext", playbackContext, allocator);
  }

  rapidjson::StringBuffer out;
  rapidjson::Writer<rapidjson::StringBuffer> writer(out);
  body.Accept(writer);
  return {out.GetString(), out.GetSize()};
}

[[nodiscard]] std::string buildSearchBody(std::string_view query)
{
  rapidjson::Document body;
  body.SetObject();
  auto& allocator = body.GetAllocator();

  rapidjson::Value context(rapidjson::kObjectType);
  rapidjson::Value client(rapidjson::kObjectType);
  client.AddMember("clientName", "WEB", allocator);
  client.AddMember("clientVersion", rapidjson::Value(WEB_CLIENT_VERSION, allocator), allocator);
  client.AddMember("hl", "en", allocator);
  client.AddMember("gl", "US", allocator);
  context.AddMember("client", client, allocator);

  body.AddMember("context", context, allocator);
  body.AddMember(
    "query",
    rapidjson::Value(query.data(), static_cast<rapidjson::SizeType>(query.size()), allocator),
    allocator);
  // Video-only filter.
  body.AddMember("params", "EgIQAQ==", allocator);

  rapidjson::StringBuffer out;
  rapidjson::Writer<rapidjson::StringBuffer> writer(out);
  body.Accept(writer);
  return {out.GetString(), out.GetSize()};
}

[[nodiscard]] std::string buildBrowseBody(std::string_view playlistId)
{
  rapidjson::Document body;
  body.SetObject();
  auto& allocator = body.GetAllocator();

  rapidjson::Value context(rapidjson::kObjectType);
  rapidjson::Value client(rapidjson::kObjectType);
  client.AddMember("clientName", "WEB", allocator);
  client.AddMember("clientVersion", rapidjson::Value(WEB_CLIENT_VERSION, allocator), allocator);
  client.AddMember("hl", "en", allocator);
  client.AddMember("gl", "US", allocator);
  context.AddMember("client", client, allocator);

  body.AddMember("context", context, allocator);

  std::string browseId = std::format("VL{}", playlistId);
  body.AddMember(
    "browseId",
    rapidjson::Value(browseId.c_str(), static_cast<rapidjson::SizeType>(browseId.size()), allocator),
    allocator);

  rapidjson::StringBuffer out;
  rapidjson::Writer<rapidjson::StringBuffer> writer(out);
  body.Accept(writer);
  return {out.GetString(), out.GetSize()};
}

[[nodiscard]] std::expected<rapidjson::Document, std::string> parseJson(std::string_view body)
{
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject())
  {
    return std::unexpected("Failed to parse JSON response");
  }
  return doc;
}

[[nodiscard]] std::expected<rapidjson::Document, std::string> postInnertube(
  std::string_view endpoint,
  std::string_view apiKey,
  std::string_view jsonBody,
  std::string_view userAgent,
  const std::vector<std::string>& extraHeaders)
{
  const std::string url = std::format(
    "https://www.youtube.com/youtubei/v1/{}?key={}&prettyPrint=false",
    endpoint,
    apiKey);

  std::vector<std::string> headers{"Content-Type: application/json", "Origin: https://www.youtube.com"};
  headers.insert(headers.end(), extraHeaders.begin(), extraHeaders.end());

  const auto response = httpRequest(
    url,
    jsonBody,
    headers,
    userAgent);
  if (!response)
  {
    return std::unexpected(response.error());
  }

  if (response->statusCode < 200 || response->statusCode >= 300)
  {
    return std::unexpected(std::format("HTTP {} from {}", response->statusCode, endpoint));
  }

  return parseJson(response->body);
}

[[nodiscard]] std::string urlEncode(std::string_view input)
{
  ensureCurlGlobalInit();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl)
  {
    return std::string(input);
  }

  char* encoded = curl_easy_escape(
    curl.get(), input.data(), static_cast<int>(input.size()));
  if (encoded == nullptr)
  {
    return std::string(input);
  }

  std::string out(encoded);
  curl_free(encoded);
  return out;
}

void collectVideoIdsFromJson(
  const rapidjson::Value& node,
  std::vector<std::string>& urls,
  std::unordered_set<std::string>& seen,
  size_t limit)
{
  if (urls.size() >= limit)
  {
    return;
  }

  if (node.IsObject())
  {
    if (node.HasMember("videoRenderer") && node["videoRenderer"].IsObject())
    {
      const auto& vr = node["videoRenderer"];
      if (auto id = jsonString(vr, "videoId"); id && isValidVideoId(*id))
      {
        if (seen.insert(*id).second)
        {
          urls.push_back(std::format("{}{}", YOUTUBE_WATCH_PREFIX, *id));
          if (urls.size() >= limit)
          {
            return;
          }
        }
      }
    }

    for (const auto& member : node.GetObject())
    {
      collectVideoIdsFromJson(member.value, urls, seen, limit);
      if (urls.size() >= limit)
      {
        return;
      }
    }
    return;
  }

  if (node.IsArray())
  {
    for (const auto& item : node.GetArray())
    {
      collectVideoIdsFromJson(item, urls, seen, limit);
      if (urls.size() >= limit)
      {
        return;
      }
    }
  }
}

void collectPlaylistVideoIdsFromJson(
  const rapidjson::Value& node,
  std::vector<std::string>& ids,
  std::unordered_set<std::string>& seen)
{
  if (node.IsObject())
  {
    auto collectId = [&](const char* key) {
      if (node.HasMember(key) && node[key].IsObject())
      {
        if (auto id = jsonString(node[key], "videoId"); id && isValidVideoId(*id))
        {
          if (seen.insert(*id).second)
          {
            ids.push_back(*id);
          }
        }
      }
    };

    collectId("playlistVideoRenderer");
    collectId("playlistPanelVideoRenderer");

    for (const auto& member : node.GetObject())
    {
      collectPlaylistVideoIdsFromJson(member.value, ids, seen);
    }
    return;
  }

  if (node.IsArray())
  {
    for (const auto& item : node.GetArray())
    {
      collectPlaylistVideoIdsFromJson(item, ids, seen);
    }
  }
}

void collectVideoIdsFromText(
  std::string_view text,
  std::vector<std::string>& urls,
  std::unordered_set<std::string>& seen,
  size_t limit)
{
  constexpr std::string_view marker = "\"videoId\":\"";

  size_t pos = 0;
  while (pos < text.size() && urls.size() < limit)
  {
    const size_t found = text.find(marker, pos);
    if (found == std::string_view::npos)
    {
      break;
    }

    const size_t idStart = found + marker.size();
    if ((idStart + 11) > text.size())
    {
      break;
    }

    std::string id(text.substr(idStart, 11));
    if (isValidVideoId(id) && seen.insert(id).second)
    {
      urls.push_back(std::format("{}{}", YOUTUBE_WATCH_PREFIX, id));
    }

    pos = idStart + 11;
  }
}

[[nodiscard]] std::vector<std::string> searchViaInnertube(std::string_view query, size_t maxResults)
{
  auto searchDoc = postInnertube(
    "search",
    WEB_API_KEY,
    buildSearchBody(query),
    WEB_USER_AGENT,
    {});
  if (!searchDoc)
  {
    logging::warn("Innertube search failed: {}", searchDoc.error());
    return {};
  }

  std::vector<std::string> urls;
  urls.reserve(maxResults);
  std::unordered_set<std::string> seen;
  seen.reserve(maxResults * 2);

  collectVideoIdsFromJson(*searchDoc, urls, seen, maxResults);
  return urls;
}

[[nodiscard]] std::vector<std::string> searchViaWebFallback(std::string_view query, size_t maxResults)
{
  std::vector<std::string> urls;
  urls.reserve(maxResults);
  std::unordered_set<std::string> seen;
  seen.reserve(maxResults * 2);

  const std::string encoded = urlEncode(query);
  const std::string searchUrl = std::format("https://www.youtube.com/results?search_query={}", encoded);

  const auto response = httpRequest(searchUrl, std::nullopt, {}, WEB_USER_AGENT);
  if (!response)
  {
    logging::warn("YouTube search page fetch failed: {}", response.error());
    return urls;
  }
  if (response->statusCode < 200 || response->statusCode >= 300)
  {
    logging::warn("Search page returned HTTP {}", response->statusCode);
    return urls;
  }

  collectVideoIdsFromText(response->body, urls, seen, maxResults);
  return urls;
}

[[nodiscard]] std::string extractTextFromRuns(const rapidjson::Value& value)
{
  if (!value.IsObject())
  {
    return {};
  }

  if (value.HasMember("simpleText") && value["simpleText"].IsString())
  {
    return std::string(value["simpleText"].GetString(), value["simpleText"].GetStringLength());
  }

  if (value.HasMember("runs") && value["runs"].IsArray())
  {
    std::string out;
    for (const auto& run : value["runs"].GetArray())
    {
      if (run.IsObject() && run.HasMember("text") && run["text"].IsString())
      {
        out.append(run["text"].GetString(), run["text"].GetStringLength());
      }
    }
    return out;
  }

  return {};
}

void collectPlaylistTitleCandidates(const rapidjson::Value& node, std::vector<std::string>& out)
{
  if (node.IsObject())
  {
    if (node.HasMember("playlistMetadataRenderer") && node["playlistMetadataRenderer"].IsObject())
    {
      if (auto title = jsonString(node["playlistMetadataRenderer"], "title");
          title && !title->empty())
      {
        out.push_back(*title);
      }
    }

    if (node.HasMember("title"))
    {
      std::string title = extractTextFromRuns(node["title"]);
      if (!title.empty())
      {
        out.push_back(std::move(title));
      }
    }

    for (const auto& member : node.GetObject())
    {
      collectPlaylistTitleCandidates(member.value, out);
    }
    return;
  }

  if (node.IsArray())
  {
    for (const auto& item : node.GetArray())
    {
      collectPlaylistTitleCandidates(item, out);
    }
  }
}

[[nodiscard]] std::optional<std::string> extractPlaylistId(std::string_view query)
{
  if (auto list = queryParam(query, "list"); list && !list->empty())
  {
    return *list;
  }

  const size_t listPos = query.find("list=");
  if (listPos == std::string_view::npos)
  {
    return std::nullopt;
  }

  std::string_view value = query.substr(listPos + 5);
  const size_t end = value.find_first_of("&#");
  if (end != std::string_view::npos)
  {
    value = value.substr(0, end);
  }
  if (value.empty())
  {
    return std::nullopt;
  }

  return percentDecode(value);
}

struct FormatCandidate
{
  std::string url;
  int64_t bitrate{0};
  bool audioOnly{false};
};

[[nodiscard]] std::optional<std::string> chooseBestStreamUrl(const rapidjson::Value& streamingData)
{
  if (!streamingData.IsObject())
  {
    return std::nullopt;
  }

  std::vector<FormatCandidate> candidates;

  auto collectFromArray = [&](const char* key) {
    if (!streamingData.HasMember(key) || !streamingData[key].IsArray())
    {
      return;
    }

    for (const auto& format : streamingData[key].GetArray())
    {
      if (!format.IsObject())
      {
        continue;
      }

      auto url = jsonString(format, "url");
      if (!url || url->empty())
      {
        continue;
      }

      const auto mime = jsonString(format, "mimeType").value_or("");
      const bool audioOnly = mime.find("audio/") != std::string::npos
                             && mime.find("video/") == std::string::npos;

      candidates.push_back(FormatCandidate{
        .url = *url,
        .bitrate = jsonInt64(format, "bitrate").value_or(0),
        .audioOnly = audioOnly,
      });
    }
  };

  collectFromArray("adaptiveFormats");
  collectFromArray("formats");

  if (!candidates.empty())
  {
    std::ranges::sort(candidates, [](const FormatCandidate& lhs, const FormatCandidate& rhs) {
      if (lhs.audioOnly != rhs.audioOnly)
      {
        return lhs.audioOnly > rhs.audioOnly;
      }
      return lhs.bitrate > rhs.bitrate;
    });
    return candidates.front().url;
  }

  if (auto hls = jsonString(streamingData, "hlsManifestUrl"); hls && !hls->empty())
  {
    return *hls;
  }

  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> playabilityError(const rapidjson::Value& root)
{
  if (!root.IsObject() || !root.HasMember("playabilityStatus") || !root["playabilityStatus"].IsObject())
  {
    return std::nullopt;
  }

  const auto& ps = root["playabilityStatus"];
  const auto status = jsonString(ps, "status").value_or("UNKNOWN");
  if (status == "OK")
  {
    return std::nullopt;
  }

  std::string reason = jsonString(ps, "reason").value_or("Playback unavailable");
  if (reason.empty())
  {
    reason = "Playback unavailable";
  }

  return std::format("{} ({})", reason, status);
}

[[nodiscard]] std::optional<ExtractedInfo> buildExtractedInfo(const rapidjson::Value& root)
{
  if (!root.IsObject())
  {
    return std::nullopt;
  }

  if (!root.HasMember("streamingData") || !root["streamingData"].IsObject())
  {
    return std::nullopt;
  }

  auto streamUrl = chooseBestStreamUrl(root["streamingData"]);
  if (!streamUrl)
  {
    return std::nullopt;
  }

  ExtractedInfo info;
  info.streamUrl = *streamUrl;

  if (root.HasMember("videoDetails") && root["videoDetails"].IsObject())
  {
    const auto& details = root["videoDetails"];
    info.title = jsonString(details, "title").value_or("");
    info.isLive = jsonBool(details, "isLiveContent").value_or(false);
    info.durationSec = jsonInt64(details, "lengthSeconds");

    if (auto id = jsonString(details, "videoId"); id && isValidVideoId(*id))
    {
      info.webpageUrl = std::format("{}{}", YOUTUBE_WATCH_PREFIX, *id);
    }
  }

  return info;
}

[[nodiscard]] std::expected<ExtractedInfo, std::string> extractViaInnertube(
  std::string_view videoId,
  const std::atomic<bool>& cancelFlag,
  size_t startClient)
{
  std::string lastError = "No extractor attempt executed";
  std::optional<WatchPageContext> watchContext;

  auto fetchedContext = fetchWatchPageContext(videoId);
  if (!fetchedContext)
  {
    logging::warn("Failed to fetch watch-page player context: {}", fetchedContext.error());
  }
  else
  {
    logging::debug(
      "Using watch-page player context: web_version={}, sts={}",
      fetchedContext->webClientVersion,
      fetchedContext->signatureTimestamp);
    watchContext = std::move(*fetchedContext);
  }

  for (size_t i = 0; i < kPlayerClients.size(); ++i)
  {
    if (cancelFlag.load(std::memory_order_acquire))
    {
      return std::unexpected("Cancelled");
    }

    const auto& client = kPlayerClients[(startClient + i) % kPlayerClients.size()];
    std::vector<std::string> playerHeaders;
    const WatchPageContext* playerContext = watchContext ? &*watchContext : nullptr;
    const std::string_view headerClientVersion = client.useLiveClientVersion && watchContext
                                                   ? std::string_view(watchContext->webClientVersion)
                                                   : std::string_view(client.clientVersion);

    playerHeaders.emplace_back(std::format("X-YouTube-Client-Name: {}", client.clientHeaderName));
    playerHeaders.emplace_back(
      std::format("X-YouTube-Client-Version: {}", headerClientVersion));
    if (watchContext && !watchContext->visitorData.empty())
    {
      playerHeaders.emplace_back(
        std::format("X-Goog-Visitor-Id: {}", watchContext->visitorData));
    }

    const std::string body = buildPlayerBody(client, videoId, playerContext);

    auto response = postInnertube("player", client.apiKey, body, client.userAgent, playerHeaders);
    if (!response)
    {
      lastError = std::format("{} client failed: {}", client.label, response.error());
      logging::warn("Innertube player ({}) failed: {}", client.label, response.error());
      continue;
    }

    if (auto err = playabilityError(*response))
    {
      lastError = std::format("{} client: {}", client.label, *err);
      logging::warn("Innertube player ({}) playability: {}", client.label, *err);
      continue;
    }

    if (auto info = buildExtractedInfo(*response))
    {
      auto streamStatus = probeHttpUrlStatus(info->streamUrl, client.userAgent);
      if (!streamStatus)
      {
        lastError = std::format("{} client stream probe failed: {}", client.label, streamStatus.error());
        logging::warn("Innertube player ({}) stream probe failed: {}", client.label, streamStatus.error());
        continue;
      }
      if (*streamStatus < 200 || *streamStatus >= 300)
      {
        lastError = std::format("{} client stream URL returned HTTP {}", client.label, *streamStatus);
        logging::warn("Innertube player ({}) stream URL returned HTTP {}", client.label, *streamStatus);
        continue;
      }

      logging::info("Selected stream via '{}' client. Title: {}", client.label, info->title);
      return *info;
    }

    lastError = std::format("{} client returned no playable URL", client.label);
    logging::warn("Innertube player ({}) response had no playable stream URL", client.label);
  }

  return std::unexpected(lastError);
}

}  // namespace

std::vector<std::string> searchCandidateUrls(
  std::string_view query,
  const std::atomic<bool>& cancelFlag,
  size_t maxResults)
{
  std::vector<std::string> candidateUrls;

  if (maxResults == 0)
  {
    return candidateUrls;
  }

  if (maxResults > MAX_RESULTS_LIMIT)
  {
    logging::debug("maxResults {} clamped to {}", maxResults, MAX_RESULTS_LIMIT);
    maxResults = MAX_RESULTS_LIMIT;
  }

  if (isLikelyUrl(query))
  {
    candidateUrls.push_back(normalizeYoutubeUrl(query));
    return candidateUrls;
  }

  if (cancelFlag.load(std::memory_order_acquire))
  {
    return candidateUrls;
  }

  candidateUrls = searchViaInnertube(query, maxResults);
  if (candidateUrls.empty())
  {
    logging::warn("Innertube search returned no candidates, falling back to web HTML parsing");
    candidateUrls = searchViaWebFallback(query, maxResults);
  }

  if (!candidateUrls.empty())
  {
    logging::info("Search returned {} candidate URLs", candidateUrls.size());
  }
  else
  {
    logging::warn("Search returned no candidates for query: {}", query);
  }

  return candidateUrls;
}

std::expected<ExtractedInfo, std::string> extractStreamInfo(
  std::string_view videoUrl,
  const std::atomic<bool>& cancelFlag,
  size_t startClient)
{
  if (cancelFlag.load(std::memory_order_acquire))
  {
    return std::unexpected("Cancelled");
  }

  auto videoId = extractVideoId(videoUrl);
  if (!videoId)
  {
    auto candidates = searchCandidateUrls(videoUrl, cancelFlag, 1);
    if (candidates.empty())
    {
      return std::unexpected("Could not resolve query to a YouTube video");
    }

    videoId = extractVideoId(candidates.front());
    if (!videoId)
    {
      return std::unexpected("Failed to extract a valid YouTube video ID");
    }
  }

  const std::string canonicalVideoUrl = std::format("{}{}", YOUTUBE_WATCH_PREFIX, *videoId);

  auto extracted = extractViaInnertube(*videoId, cancelFlag, startClient);
  if (!extracted)
  {
    return std::unexpected(extracted.error());
  }

  if (extracted->webpageUrl.empty())
  {
    extracted->webpageUrl = canonicalVideoUrl;
  }
  if (extracted->title.empty())
  {
    extracted->title = std::format("YouTube video {}", *videoId);
  }

  return extracted;
}

std::optional<PlaylistInfo> extractPlaylistInfo(std::string_view query)
{
  auto playlistId = extractPlaylistId(query);
  if (!playlistId || playlistId->empty())
  {
    return std::nullopt;
  }

  logging::info("Detected playlist URL, extracting entries...");

  PlaylistInfo info;
  info.playlistTitle = "YouTube Playlist";

  std::vector<std::string> playlistVideoIds;
  std::unordered_set<std::string> seen;
  seen.reserve(256);

  auto browseDoc = postInnertube(
    "browse",
    WEB_API_KEY,
    buildBrowseBody(*playlistId),
    WEB_USER_AGENT,
    {});
  if (browseDoc)
  {
    collectPlaylistVideoIdsFromJson(*browseDoc, playlistVideoIds, seen);

    std::vector<std::string> titleCandidates;
    collectPlaylistTitleCandidates(*browseDoc, titleCandidates);
    for (const auto& candidate : titleCandidates)
    {
      if (!candidate.empty())
      {
        info.playlistTitle = candidate;
        break;
      }
    }
  }
  else
  {
    logging::warn("Innertube playlist browse failed: {}", browseDoc.error());
  }

  if (playlistVideoIds.empty())
  {
    const std::string playlistUrl = std::format(
      "https://www.youtube.com/playlist?list={}",
      urlEncode(*playlistId));
    const auto response = httpRequest(playlistUrl, std::nullopt, {}, WEB_USER_AGENT);
    if (response && response->statusCode >= 200 && response->statusCode < 300)
    {
      std::vector<std::string> urls;
      urls.reserve(300);
      collectVideoIdsFromText(response->body, urls, seen, 300);

      playlistVideoIds.reserve(urls.size());
      for (const auto& url : urls)
      {
        if (auto id = extractVideoId(url); id)
        {
          playlistVideoIds.push_back(*id);
        }
      }
    }
  }

  if (playlistVideoIds.empty())
  {
    logging::warn("Playlist extraction returned zero entries");
    return std::nullopt;
  }

  info.videoUrls.reserve(playlistVideoIds.size());
  for (const auto& id : playlistVideoIds)
  {
    info.videoUrls.push_back(std::format("{}{}", YOUTUBE_WATCH_PREFIX, id));
  }

  logging::info(
    "Extracted {} videos from playlist '{}'",
    info.videoUrls.size(),
    info.playlistTitle);

  return info;
}