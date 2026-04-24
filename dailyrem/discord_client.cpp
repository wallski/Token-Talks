#include "discord_client.h"
#include "vendor/nlohmann/json.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#define NOMINMAX
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#include "gui.h"
#include "vendor/include/opus.h"
#include "vendor/include/sodium.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "opus.lib")
#pragma comment(lib, "libsodium.lib")

using json = nlohmann::json;

DiscordClient::DiscordClient()
    : m_Connected(false), m_RunHeartbeat(false), m_HeartbeatInterval(41250),
      m_SequenceNumber(0), m_hWebSocket(nullptr), m_SendThreadRunning(false) {
  m_SendThreadRunning = true;
  m_SendThread = std::thread(&DiscordClient::SendThreadLoop, this);
}

DiscordClient::~DiscordClient() {
  Disconnect();
  m_SendThreadRunning = false;
  m_SendCv.notify_all();
  if (m_SendThread.joinable())
    m_SendThread.join();
}

void DiscordClient::SetToken(const std::string &token) { m_Token = token; }

std::string DiscordClient::GetToken() const { return m_Token; }

std::string DiscordClient::GetUserId() const { return m_UserId; }
std::string DiscordClient::GetUserName() const { return m_DisplayName; }
std::string DiscordClient::GetUserAvatar() const { return m_AvatarHash; }

bool DiscordClient::ValidateToken(const std::string &token) {
  DiscordClient temp;
  temp.SetToken(token);
  std::string resp = temp.HttpRequest("GET", "/api/v9/users/@me");
  if (resp.empty())
    return false;
  try {
    auto j = json::parse(resp);
    if (j.contains("id") && j.contains("username"))
      return true;
  } catch (...) {
  }
  return false;
}

std::string DiscordClient::LoginWithCredentials(const std::string &email,
                                                const std::string &password,
                                                std::string &out_mfa_ticket) {
  DiscordClient temp;
  json payload = {{"login", email},          {"password", password},
                  {"undelete", false},       {"captcha_key", nullptr},
                  {"login_source", nullptr}, {"gift_code_sku_id", nullptr}};
  std::string resp =
      temp.HttpRequest("POST", "/api/v9/auth/login", payload.dump());
  if (resp.empty())
    return "";
  try {
    auto j = json::parse(resp);
    if (j.contains("token") && !j["token"].is_null())
      return j["token"].get<std::string>();
    if (j.contains("mfa") && j["mfa"].get<bool>() && j.contains("ticket")) {
      out_mfa_ticket = j["ticket"].get<std::string>();
      return "";
    }
  } catch (...) {
  }
  return "";
}

std::string DiscordClient::SubmitMfaCode(const std::string &code,
                                         const std::string &ticket) {
  DiscordClient temp;
  json payload = {{"code", code}, {"ticket", ticket}};
  std::string resp =
      temp.HttpRequest("POST", "/api/v9/auth/mfa/totp", payload.dump());
  if (resp.empty())
    return "";
  try {
    auto j = json::parse(resp);
    if (j.contains("token") && !j["token"].is_null())
      return j["token"].get<std::string>();
  } catch (...) {
  }
  return "";
}

bool DiscordClient::Connect() {
  if (m_Token.empty())
    return false;
  if (m_Connected)
    return true;

  std::string resp = HttpRequest("GET", "/api/v9/users/@me");
  if (!resp.empty()) {
    try {
      auto j = json::parse(resp);
      if (j.contains("id"))
        m_UserId = j["id"].get<std::string>();
      if (j.contains("global_name") && !j["global_name"].is_null())
        m_DisplayName = j["global_name"].get<std::string>();
      else if (j.contains("username"))
        m_DisplayName = j["username"].get<std::string>();
      if (j.contains("avatar") && !j["avatar"].is_null())
        m_AvatarHash = j["avatar"].get<std::string>();
    } catch (...) {
    }
  }

  m_Connected = true;
  m_WsThread = std::thread(&DiscordClient::WebSocketLoop, this);
  return true;
}

void DiscordClient::Disconnect() {
  m_Connected = false;
  m_RunHeartbeat = false;

  // Close the websocket handle to unblock any pending WinHttp receive calls.
  // Do NOT join threads here — ExitProcess(0) in RunGUI will terminate them
  // instantly, which is what we want to avoid the EXE lock on recompile.
  {
    std::lock_guard<std::mutex> lock(m_WsMutex);
    if (m_hWebSocket) {
      WinHttpCloseHandle((HINTERNET)m_hWebSocket);
      m_hWebSocket = nullptr;
    }
  }

  if (m_WsThread.joinable())
    m_WsThread.detach();
  if (m_HeartbeatThread.joinable())
    m_HeartbeatThread.detach();
}

void DiscordClient::SetOnMessageCallback(
    std::function<void(const DiscordMessage &)> cb) {
  m_MessageCallback = cb;
}

void DiscordClient::SetOnConnectedCallback(std::function<void()> cb) {
  m_ConnectedCallback = cb;
}

bool DiscordClient::IsConnected() const { return m_Connected; }

std::string DiscordClient::HttpRequest(const std::string &method,
                                       const std::string &path,
                                       const std::string &body) {
  HINTERNET session =
      WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return "";

  HINTERNET connect =
      WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return "";
  }

  std::wstring wPath(path.begin(), path.end());
  std::wstring wMethod(method.begin(), method.end());

  HINTERNET request = WinHttpOpenRequest(
      connect, wMethod.c_str(), wPath.c_str(), NULL, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return "";
  }

  std::string authHeader = "Authorization: " + m_Token + "\r\n";
  std::string contentHeader = "Content-Type: application/json\r\n";
  std::string combinedHeaders = authHeader + contentHeader;
  std::wstring wheaders(combinedHeaders.begin(), combinedHeaders.end());

  BOOL sent =
      WinHttpSendRequest(request, wheaders.c_str(), -1, (LPVOID)body.c_str(),
                         (DWORD)body.size(), (DWORD)body.size(), 0);
  if (!sent) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return "";
  }

  WinHttpReceiveResponse(request, NULL);

  DWORD statusCode = 0;
  DWORD size = sizeof(statusCode);
  WinHttpQueryHeaders(request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      NULL, &statusCode, &size, NULL);

  std::string responseBody;
  DWORD bytesAvailable = 0;
  while (WinHttpQueryDataAvailable(request, &bytesAvailable) &&
         bytesAvailable > 0) {
    std::vector<char> buffer(bytesAvailable);
    DWORD bytesRead = 0;
    if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead)) {
      responseBody.append(buffer.data(), bytesRead);
    }
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  return responseBody;
}

std::vector<DiscordGuild> DiscordClient::FetchGuilds() {
  std::vector<DiscordGuild> guilds;
  std::string resp = HttpRequest("GET", "/api/v9/users/@me/guilds");
  if (resp.empty())
    return guilds;

  try {
    auto j = json::parse(resp);
    if (j.is_array()) {
      for (const auto &item : j) {
        DiscordGuild g;
        g.id = item["id"].get<std::string>();
        g.name = item["name"].get<std::string>();
        if (item.contains("icon") && !item["icon"].is_null())
          g.icon_hash = item["icon"].get<std::string>();
        guilds.push_back(g);
      }
    }
  } catch (...) {
  }
  return guilds;
}

std::vector<DiscordChannel>
DiscordClient::FetchChannels(const std::string &guild_id) {
  std::vector<DiscordChannel> channels;
  std::string resp =
      HttpRequest("GET", "/api/v9/guilds/" + guild_id + "/channels");
  if (resp.empty())
    return channels;

  try {
    auto j = json::parse(resp);
    if (j.is_array()) {
      std::map<std::string, bool> category_locked;

      auto isNodeLocked = [&](const json &item) -> bool {
        if (item.contains("permission_overwrites") &&
            item["permission_overwrites"].is_array()) {
          for (const auto &ow : item["permission_overwrites"]) {
            if (ow.contains("id") && ow["id"].is_string() &&
                ow["id"] == guild_id) {
              if (ow.contains("deny")) {
                std::string deny_str =
                    ow["deny"].is_string()
                        ? ow["deny"].get<std::string>()
                        : std::to_string(ow["deny"].get<long long>());
                if (!deny_str.empty()) {
                  unsigned long long deny_bits = std::stoull(deny_str);
                  if ((deny_bits & 1024ULL) != 0) {
                    return true;
                  }
                }
              }
            }
          }
        }
        return false;
      };

      for (const auto &item : j) {
        if (item["type"].get<int>() == 4) {
          category_locked[item["id"].get<std::string>()] = isNodeLocked(item);
        }
      }

      for (const auto &item : j) {
        int type = item["type"].get<int>();
        if (type == 0 || type == 2) {
          bool locked = isNodeLocked(item);
          if (!locked && item.contains("parent_id") &&
              !item["parent_id"].is_null()) {
            std::string parent_id = item["parent_id"].get<std::string>();
            if (category_locked[parent_id]) {
              locked = true;
            }
          }
          DiscordChannel ch;
          ch.id = item["id"].get<std::string>();
          ch.name = item["name"].get<std::string>();
          ch.type = type;
          ch.is_locked = locked;
          ch.position =
              item.contains("position") ? item["position"].get<int>() : 0;
          ch.parent_id =
              (item.contains("parent_id") && !item["parent_id"].is_null())
                  ? item["parent_id"].get<std::string>()
                  : "";
          channels.push_back(ch);
        }
      }
    }
  } catch (...) {
  }
  return channels;
}

std::vector<DiscordChannel> DiscordClient::FetchPrivateChannels() {
  std::vector<DiscordChannel> channels;
  std::string resp = HttpRequest("GET", "/api/v9/users/@me/channels");
  if (resp.empty())
    return channels;

  try {
    auto j = json::parse(resp);
    if (j.is_array()) {
      for (const auto &item : j) {
        std::string name = "";
        if (item.contains("name") && !item["name"].is_null()) {
          name = item["name"].get<std::string>();
        } else if (item.contains("recipients") &&
                   item["recipients"].is_array() &&
                   !item["recipients"].empty()) {
          // Join recipient display names for DM/Group DM
          for (size_t i = 0; i < item["recipients"].size(); ++i) {
            if (i > 0)
              name += ", ";
            const auto &rec = item["recipients"][i];
            // prefer global_name (display name) over username
            if (rec.contains("global_name") && !rec["global_name"].is_null())
              name += rec["global_name"].get<std::string>();
            else
              name += rec["username"].get<std::string>();
          }
        }

        if (name.empty())
          name = "Unnamed DM";

        channels.push_back({item["id"].get<std::string>(), name,
                            item["type"].get<int>(), false});
      }
    }
  } catch (...) {
  }
  return channels;
}

std::vector<DiscordMessage>
DiscordClient::FetchMessages(const std::string &channel_id,
                             const std::string &before_id) {
  std::vector<DiscordMessage> msgs;
  std::string endpoint =
      "/api/v9/channels/" + channel_id + "/messages?limit=50";
  if (!before_id.empty())
    endpoint += "&before=" + before_id;
  std::string resp = HttpRequest("GET", endpoint);
  if (resp.empty())
    return msgs;

  try {
    auto j = json::parse(resp);
    if (j.is_array()) {
      for (auto it = j.rbegin(); it != j.rend();
           ++it) { // Reverse to get oldest to newest
        DiscordMessage dmsg;
        ParseJsonMessage(*it, dmsg);
        msgs.push_back(dmsg);
      }
    }
  } catch (...) {
  }
  return msgs;
}

bool DiscordClient::SendDiscordMessage(const std::string &channel_id,
                                       const std::string &content) {
  json payload = {{"content", content}};
  std::string resp = HttpRequest(
      "POST", "/api/v9/channels/" + channel_id + "/messages", payload.dump());
  return !resp.empty();
}

bool DiscordClient::EditMessage(const std::string &channel_id,
                                const std::string &msg_id,
                                const std::string &new_content) {
  json payload = {{"content", new_content}};
  std::string resp = HttpRequest(
      "PATCH", "/api/v9/channels/" + channel_id + "/messages/" + msg_id,
      payload.dump());
  return !resp.empty();
}

bool DiscordClient::DeleteMessage(const std::string &channel_id,
                                  const std::string &msg_id) {
  std::string resp = HttpRequest("DELETE", "/api/v9/channels/" + channel_id +
                                               "/messages/" + msg_id);
  return !resp.empty();
}

bool DiscordClient::AddReaction(const std::string &channel_id,
                                const std::string &msg_id,
                                const std::string &emoji) {
  std::string path = "/api/v9/channels/" + channel_id + "/messages/" + msg_id +
                     "/reactions/" + emoji + "/@me";
  std::string resp = HttpRequest("PUT", path, "");
  return true;
}

std::vector<unsigned char>
DiscordClient::DownloadFile(const std::string &urlStr) {
  std::vector<unsigned char> data;
  URL_COMPONENTS urlComp;
  ZeroMemory(&urlComp, sizeof(urlComp));
  urlComp.dwStructSize = sizeof(urlComp);
  wchar_t hostName[256];
  wchar_t urlPath[1024];
  urlComp.lpszHostName = hostName;
  urlComp.dwHostNameLength = 256;
  urlComp.lpszUrlPath = urlPath;
  urlComp.dwUrlPathLength = 1024;

  std::wstring wUrl(urlStr.begin(), urlStr.end());
  if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp))
    return data;

  HINTERNET hSession =
      WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET hConnect =
      WinHttpConnect(hSession, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET hRequest =
      WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

  if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    if (WinHttpReceiveResponse(hRequest, NULL)) {
      DWORD bytesAvailable = 0;
      while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) &&
             bytesAvailable > 0) {
        std::vector<unsigned char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable,
                            &bytesRead)) {
          data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
        }
      }
    }
  }
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return data;
}

#include <fstream>
bool DiscordClient::SendAttachment(const std::string &channel_id,
                                   const std::string &filepath) {
  if (m_Token.empty())
    return false;

  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<char> fileBuffer(size);
  if (!file.read(fileBuffer.data(), size))
    return false;

  std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);

  std::string body;
  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"payload_json\"\r\n\r\n";
  body += "{\"content\":\"\"}\r\n";

  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"files[0]\"; filename=\"" +
          filename + "\"\r\n";
  body += "Content-Type: application/octet-stream\r\n\r\n";

  std::vector<char> requestBuffer(body.begin(), body.end());
  requestBuffer.insert(requestBuffer.end(), fileBuffer.begin(),
                       fileBuffer.end());

  std::string endBoundary = "\r\n--" + boundary + "--\r\n";
  requestBuffer.insert(requestBuffer.end(), endBoundary.begin(),
                       endBoundary.end());

  HINTERNET session =
      WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect =
      WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);

  std::string path = "/api/v9/channels/" + channel_id + "/messages";
  std::wstring wPath(path.begin(), path.end());
  HINTERNET request = WinHttpOpenRequest(
      connect, L"POST", wPath.c_str(), NULL, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

  std::string headers = "Authorization: " + m_Token + "\r\n";
  headers += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
  std::wstring wheaders(headers.begin(), headers.end());

  BOOL sent = WinHttpSendRequest(
      request, wheaders.c_str(), -1, requestBuffer.data(),
      (DWORD)requestBuffer.size(), (DWORD)requestBuffer.size(), 0);
  if (sent)
    WinHttpReceiveResponse(request, NULL);

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return sent == TRUE;
}

void DiscordClient::ParseJsonMessage(const json &item, DiscordMessage &dmsg) {
  dmsg.id = item["id"].get<std::string>();
  if (item.contains("author") && !item["author"].is_null()) {
    const auto &au = item["author"];
    if (au.contains("username"))
      dmsg.author_username = au["username"].get<std::string>();
    if (au.contains("id"))
      dmsg.author_id = au["id"].get<std::string>();
    if (au.contains("avatar") && !au["avatar"].is_null())
      dmsg.author_avatar = au["avatar"].get<std::string>();
    // Prefer global_name (display name) over raw username
    if (au.contains("global_name") && !au["global_name"].is_null())
      dmsg.author = au["global_name"].get<std::string>();
    else
      dmsg.author = dmsg.author_username;
  }

  if (item.contains("timestamp"))
    dmsg.timestamp = item["timestamp"].get<std::string>();

  if (item.contains("content"))
    dmsg.content = item["content"].get<std::string>();

  if (item.contains("attachments") && item["attachments"].is_array()) {
    for (const auto &att : item["attachments"]) {
      if (att.contains("url")) {
        std::string url = att["url"].get<std::string>();
        std::string fname =
            att.contains("filename") ? att["filename"].get<std::string>() : "";
        std::string ext =
            fname.size() >= 4 ? fname.substr(fname.size() - 4) : "";
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".jpg" || ext == "jpeg" || ext == ".gif" ||
            ext == ".webp") {
          dmsg.attachment_urls.push_back(url);
        } else if (ext == ".mp4" || ext == ".mov" || ext == ".webm" ||
                   ext == ".m4v") {
          dmsg.video_urls.push_back(url);
        }
      }
    }
  }

  if (dmsg.attachment_urls.empty() && item.contains("embeds") &&
      item["embeds"].is_array()) {
    for (const auto &emb : item["embeds"]) {
      if (emb.contains("image") && emb["image"].contains("url")) {
        dmsg.attachment_urls.push_back(emb["image"]["url"].get<std::string>());
      } else if (emb.contains("thumbnail") &&
                 emb["thumbnail"].contains("url")) {
        dmsg.attachment_urls.push_back(
            emb["thumbnail"]["url"].get<std::string>());
      }
    }
  }

  if (item.contains("reactions") && item["reactions"].is_array()) {
    for (const auto &r : item["reactions"]) {
      DiscordReaction dr;
      if (r.contains("emoji") && r["emoji"].contains("name"))
        dr.emoji = r["emoji"]["name"].get<std::string>();
      dr.count = r.contains("count") ? r["count"].get<int>() : 1;
      dr.me = r.contains("me") ? r["me"].get<bool>() : false;
      dmsg.reactions.push_back(dr);
    }
  }
}

void DiscordClient::SendIdentify(void *hWebSocket) {
  json identify = {
      {"op", 2},
      {"d",
       {{"token", m_Token},
        {"capabilities", 16381},
        {"properties",
         {{"os", "Windows"}, {"browser", "Chrome"}, {"device", ""}}}}}};
  QueueWsMessage(identify.dump());
}

void DiscordClient::WebSocketLoop() {
  HINTERNET hSession =
      WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET hConnect = WinHttpConnect(hSession, L"gateway.discord.gg",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, L"GET", L"/?v=9&encoding=json", NULL, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
  WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  WinHttpReceiveResponse(hRequest, NULL);

  HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
  WinHttpCloseHandle(hRequest);

  if (!hWebSocket) {
    m_Connected = false;
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_WsMutex);
    m_hWebSocket = hWebSocket;
  }

  // Allocate buffer
  const DWORD cbBuffer = 65536;
  BYTE *pbBuffer = new BYTE[cbBuffer];

  while (m_Connected) {
    DWORD cbDataRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType;
    DWORD dwError = WinHttpWebSocketReceive(hWebSocket, pbBuffer, cbBuffer,
                                            &cbDataRead, &eBufferType);

    if (dwError != ERROR_SUCCESS)
      break;
    if (eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
      break;

    if (cbDataRead > 0) {
      std::string msg((char *)pbBuffer, cbDataRead);
      try {
        auto j = json::parse(msg);
        int op = j["op"].get<int>();

        if (j.contains("s") && !j["s"].is_null()) {
          m_SequenceNumber = j["s"].get<int>();
        }

        if (op == 10) { // Hello
          m_HeartbeatInterval = j["d"]["heartbeat_interval"].get<int>();
          m_RunHeartbeat = true;
          m_HeartbeatThread = std::thread(&DiscordClient::HeartbeatLoop, this);
          SendIdentify(hWebSocket);
        } else if (op == 0) { // Dispatch
          std::string t = j["t"].get<std::string>();
          if (t == "READY") {
            // Capture our session_id — required for voice Identify (Op 0)
            if (j["d"].contains("session_id") &&
                !j["d"]["session_id"].is_null())
              m_SessionId = j["d"]["session_id"].get<std::string>();
            if (m_ConnectedCallback)
              m_ConnectedCallback();
          } else if (t == "MESSAGE_CREATE") {
            if (m_MessageCallback) {
              DiscordMessage dmsg;
              ParseJsonMessage(j["d"], dmsg);
              m_MessageCallback(dmsg);
            }
          } else if (t == "GUILD_CREATE") {
            if (j["d"].contains("voice_states") &&
                j["d"]["voice_states"].is_array()) {
              for (const auto &vs : j["d"]["voice_states"]) {
                ParseVoiceStateUpdate(vs);
              }
            }
          } else if (t == "VOICE_STATE_UPDATE") {
            ParseVoiceStateUpdate(j["d"]);
          } else if (t == "VOICE_SERVER_UPDATE") {
            auto d = j["d"];
            if (d["endpoint"].is_null())
              continue; // Server is currently unavailable

            m_VoiceConn.m_Endpoint = d["endpoint"].get<std::string>();
            m_VoiceConn.m_Token = d["token"].get<std::string>();
            m_VoiceConn.m_GuildId = d["guild_id"].get<std::string>();

            if (m_VoiceConn.m_Running) {
              m_VoiceConn.m_Running = false;
              // Don't join here, it blocks the gateway loop.
              // The loop should exit on its own when m_Running is false.
              // If we really need to wait, we'd do it on a separate task
              // thread.
            }

            if (m_VoiceConn.m_VoiceThread.joinable()) {
              m_VoiceConn.m_VoiceThread
                  .detach(); // Detach to allow new connection
            }

            m_VoiceConn.m_Running = true;
            m_VoiceConn.m_VoiceThread =
                std::thread(&DiscordClient::VoiceLoop, this,
                            m_VoiceConn.m_Endpoint, m_VoiceConn.m_Token,
                            m_VoiceConn.m_GuildId, m_SessionId, m_UserId);
          }
        }
      } catch (...) {
      }
    }
  }

  delete[] pbBuffer;

  {
    std::lock_guard<std::mutex> lock(m_WsMutex);
    if (m_hWebSocket) {
      WinHttpWebSocketClose((HINTERNET)m_hWebSocket,
                            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
      WinHttpCloseHandle((HINTERNET)m_hWebSocket);
      m_hWebSocket = nullptr;
    }
  }

  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  m_Connected = false;
}

bool DiscordClient::JoinVoiceChannel(const std::string &guild_id,
                                     const std::string &channel_id) {
  m_VoiceReady = false;
  json payload = {
      {"op", 4},
      {"d",
       {{"guild_id", guild_id},
        {"channel_id", (channel_id.empty() ? nullptr : json(channel_id))},
        {"self_mute", false},
        {"self_deaf", false}}}};
  QueueWsMessage(payload.dump());
  return true;
}

void DiscordClient::LeaveVoiceChannel(const std::string &guild_id) {
  JoinVoiceChannel(guild_id, ""); // Sending null channel ID leaves the channel
  m_VoiceConn.m_Running = false;
  // Don't join here to avoid freezing the UI thread.
}

void DiscordClient::SetVoiceState(bool muted, bool deafened) {
  m_VoiceConn.m_IsMuted = muted;
  m_VoiceConn.m_IsDeafened = deafened;
  // Optionally send op 5 (Speaking) to Discord if we were to implement talking
  // indicators
}

void DiscordClient::SetAudioDevices(int inputIdx, int outputIdx) {
  m_VoiceConn.m_InputDevice = inputIdx;
  m_VoiceConn.m_OutputDevice = outputIdx;
}

std::vector<VoiceMember>
DiscordClient::GetVoiceMembers(const std::string &channel_id) {
  std::lock_guard<std::mutex> lock(m_VoiceMutex);
  return m_VoiceMembers;
}

void DiscordClient::VoiceLoop(std::string endpoint, std::string token,
                              std::string guildId, std::string sessionId,
                              std::string userId) {
  if (endpoint.find("wss://") == 0)
    endpoint = endpoint.substr(6);
  size_t colon = endpoint.find(':');
  if (colon != std::string::npos)
    endpoint = endpoint.substr(0, colon);

  HINTERNET hSession =
      WinHttpOpen(L"DiscordVoice/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession)
    return;
  HINTERNET hConnect = WinHttpConnect(
      hSession, std::wstring(endpoint.begin(), endpoint.end()).c_str(), 443, 0);
  HINTERNET hRequest =
      WinHttpOpenRequest(hConnect, L"GET", L"/?v=4", NULL, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

  WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(hRequest, NULL)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return;
  }

  HINTERNET hVoiceWS = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
  WinHttpCloseHandle(hRequest);
  if (!hVoiceWS) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return;
  }

  m_VoiceConn.m_hVoiceWS = hVoiceWS;

  json identify = {{"op", 0},
                   {"d",
                    {{"server_id", guildId},
                     {"user_id", userId},
                     {"session_id", sessionId},
                     {"token", token}}}};
  std::string sId = identify.dump();
  WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                       (void *)sId.c_str(), (DWORD)sId.size());

  SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
  int timeout = 2000;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
             sizeof(timeout));
  m_VoiceConn.m_UdpSocket = udpSocket;

  const DWORD cbBuffer = 8192;
  BYTE *pbBuffer = new BYTE[cbBuffer];

  while (m_VoiceConn.m_Running) {
    DWORD cbRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    std::string wsMessage;

    do {
      if (WinHttpWebSocketReceive(hVoiceWS, pbBuffer, cbBuffer, &cbRead,
                                  &type) != ERROR_SUCCESS) {
        OutputDebugStringA("[VOICE] WS Receive Error\n");
        break;
      }
      wsMessage.append((char *)pbBuffer, cbRead);
    } while (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
             type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);

    if (wsMessage.empty())
      break;

    try {
      auto j = json::parse(wsMessage);
      int op = j["op"];
      auto d = j["d"];

      if (op == 2) { // READY
        OutputDebugStringA("[VOICE] Received READY (Op 2)\n");
        uint32_t ssrc = d["ssrc"].get<uint32_t>();
        m_VoiceConn.m_Ssrc = ssrc; // BUG FIX: was never stored
        std::string ip = d["ip"].get<std::string>();
        int port = d["port"].get<int>();

        // BUG FIX: resolve address BEFORE sending any packets
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints,
                        &res) == 0 &&
            res) {
          m_VoiceConn.m_ServerAddr = *(struct sockaddr_in *)res->ai_addr;
          freeaddrinfo(res);
        } else {
          // Fallback: manual inet_pton
          m_VoiceConn.m_ServerAddr = {};
          m_VoiceConn.m_ServerAddr.sin_family = AF_INET;
          m_VoiceConn.m_ServerAddr.sin_port = htons((u_short)port);
          inet_pton(AF_INET, ip.c_str(), &m_VoiceConn.m_ServerAddr.sin_addr);
        }

        // IP Discovery packet (type=1, length=70, ssrc)
        unsigned char packet[74] = {0};
        *(uint16_t *)(packet) = htons(1);
        *(uint16_t *)(packet + 2) = htons(70);
        *(uint32_t *)(packet + 4) = htonl(ssrc);

        bool discovered = false;
        for (int retry = 0; retry < 5 && !discovered; ++retry) {
          sendto(udpSocket, (char *)packet, 74, 0,
                 (struct sockaddr *)&m_VoiceConn.m_ServerAddr,
                 sizeof(m_VoiceConn.m_ServerAddr));
          struct sockaddr_in from;
          int fromLen = sizeof(from);
          char resp2[74] = {0};
          int r = recvfrom(udpSocket, resp2, 74, 0, (struct sockaddr *)&from,
                           &fromLen);
          if (r > 0) {
            discovered = true;
            // Safe IP parsing from discovery response
            char szIp[64] = {0};
            for (int k = 0; k < 63; k++) {
              char c = resp2[8 + k];
              if (c == 0)
                break;
              szIp[k] = c;
            }
            std::string myIp = szIp;
            uint16_t myPort = ntohs(*(uint16_t *)(resp2 + 72));
            json selectP = {{"op", 1},
                            {"d",
                             {{"protocol", "udp"},
                              {"data",
                               {{"address", myIp},
                                {"port", (int)myPort},
                                {"mode", "xsalsa20_poly1305"}}}}}};
            OutputDebugStringA(
                "[VOICE] Discovery OK, sending Select Protocol\n");
            std::string sSel = selectP.dump();
            WinHttpWebSocketSend(hVoiceWS,
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void *)sSel.c_str(), (DWORD)sSel.size());
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
        }
        if (!discovered) {
          // Fallback: send Select Protocol with server IP directly
          OutputDebugStringA(
              "[VOICE] Discovery failed, sending fallback Select Protocol\n");
          json selectP = {{"op", 1},
                          {"d",
                           {{"protocol", "udp"},
                            {"data",
                             {{"address", ip},
                              {"port", port},
                              {"mode", "xsalsa20_poly1305"}}}}}};
          std::string sSel = selectP.dump();
          WinHttpWebSocketSend(hVoiceWS,
                               WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               (void *)sSel.c_str(), (DWORD)sSel.size());
        }
      } else if (op == 4) { // SESSION_DESCRIPTION
        OutputDebugStringA(
            "[VOICE] Received SESSION_DESCRIPTION (Op 4) - READY!\n");
        m_VoiceConn.m_SecretKey = d["secret_key"].get<std::vector<uint8_t>>();
        m_VoiceConn.m_Ready = true;
        m_VoiceReady = true;
        std::thread(&DiscordClient::AudioCaptureLoop, this).detach();
      } else if (op == 8) { // HELLO
        int interval = d["heartbeat_interval"];
        std::thread([this, hVoiceWS, interval]() {
          while (m_VoiceConn.m_Running && m_VoiceConn.m_hVoiceWS == hVoiceWS) {
            json hb = {{"op", 3}, {"d", GetTickCount()}};
            std::string s = hb.dump();
            WinHttpWebSocketSend(hVoiceWS,
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void *)s.c_str(), (DWORD)s.size());
            for (int i = 0; i < interval && m_VoiceConn.m_Running; i += 100)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }).detach();
      }
    } catch (...) {
    }
  }
  m_VoiceReady = false;
  m_VoiceConn.m_Ready = false;
  delete[] pbBuffer;
  closesocket(udpSocket);
  WinHttpWebSocketClose(hVoiceWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL,
                        0);
  WinHttpCloseHandle(hVoiceWS);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
}

void DiscordClient::AudioCaptureLoop() {
  // 1. Tell Discord we are speaking
  json speaking = {
      {"op", 5},
      {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", m_VoiceConn.m_Ssrc}}}};
  std::string sSp = speaking.dump();
  if (m_VoiceConn.m_hVoiceWS)
    WinHttpWebSocketSend(m_VoiceConn.m_hVoiceWS,
                         WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                         (void *)sSp.c_str(), (DWORD)sSp.size());

  int samples = 48000;
  int channels = 1;
  OpusEncoder *encoder =
      opus_encoder_create(samples, channels, OPUS_APPLICATION_VOIP, nullptr);
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));

  uint16_t seq = 0;
  uint32_t ts = 0;
  unsigned char rtp_packet[1024];
  unsigned char silent_opus[3] = {0xF8, 0xFF, 0xFE}; // Opus silent frame

  while (m_VoiceConn.m_Running && m_VoiceReady) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (m_VoiceConn.m_IsMuted)
      continue;

    // RTP Header
    rtp_packet[0] = 0x80;
    rtp_packet[1] = 0x78;
    *(uint16_t *)(rtp_packet + 2) = htons(seq++);
    *(uint32_t *)(rtp_packet + 4) = htonl(ts);
    *(uint32_t *)(rtp_packet + 8) = htonl(m_VoiceConn.m_Ssrc);
    ts += 960;

    // Nonce for xsalsa20_poly1305_suffix (24 random bytes)
    unsigned char nonce[24] = {0};
    for (int i = 0; i < 24; ++i) nonce[i] = rand() % 256;

    // Encrypt (libsodium)
    unsigned char encrypted[512] = {0};
    unsigned long long encrypted_len = 0;

    if (m_VoiceConn.m_SecretKey.size() == 32) {
      crypto_secretbox_easy(encrypted, silent_opus, 3, nonce,
                            m_VoiceConn.m_SecretKey.data());
      encrypted_len = 3 + crypto_secretbox_MACBYTES;

      // Append encrypted data to RTP header
      memcpy(rtp_packet + 12, encrypted, (size_t)encrypted_len);
      // Append the 24-byte nonce suffix
      memcpy(rtp_packet + 12 + encrypted_len, nonce, 24);
      
      sendto(m_VoiceConn.m_UdpSocket, (char *)rtp_packet,
             12 + (int)encrypted_len + 24, 0,
             (struct sockaddr *)&m_VoiceConn.m_ServerAddr,
             sizeof(m_VoiceConn.m_ServerAddr));
    }
  }
  opus_encoder_destroy(encoder);
}

void DiscordClient::HeartbeatLoop() {
  while (m_RunHeartbeat) {
    json heartbeat = {
        {"op", 1},
        {"d", m_SequenceNumber > 0 ? json(m_SequenceNumber) : nullptr}};
    QueueWsMessage(heartbeat.dump());

    for (int i = 0; i < m_HeartbeatInterval && m_RunHeartbeat; i += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void DiscordClient::QueueWsMessage(const std::string &message) {
  std::lock_guard<std::mutex> lock(m_SendMutex);
  m_WsSendQueue.push(message);
  m_SendCv.notify_one();
}

void DiscordClient::SendThreadLoop() {
  while (m_SendThreadRunning) {
    std::string msg;
    {
      std::unique_lock<std::mutex> lock(m_SendMutex);
      m_SendCv.wait(lock, [this] {
        return !m_WsSendQueue.empty() || !m_SendThreadRunning;
      });
      if (!m_SendThreadRunning)
        break;
      msg = m_WsSendQueue.front();
      m_WsSendQueue.pop();
    }

    HINTERNET hWS = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_WsMutex);
      if (m_Connected)
        hWS = (HINTERNET)m_hWebSocket;
    }

    if (hWS) {
      WinHttpWebSocketSend(hWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                           (void *)msg.c_str(), (DWORD)msg.size());
    }
  }
}

void DiscordClient::ParseVoiceStateUpdate(const nlohmann::json &d) {
  std::string userId = d.value("user_id", "");
  std::string channelId =
      (d.contains("channel_id") && !d["channel_id"].is_null())
          ? d["channel_id"].get<std::string>()
          : "";
  // Try to grab display name and avatar from the embedded member/user object
  std::string displayName = "User " + userId.substr(0, 4);
  std::string avatarHash = "";
  std::string username = "";
  if (d.contains("member") && !d["member"].is_null()) {
    const auto &mem = d["member"];
    if (mem.contains("nick") && !mem["nick"].is_null())
      displayName = mem["nick"].get<std::string>();
    if (mem.contains("user") && !mem["user"].is_null()) {
      const auto &u = mem["user"];
      if (mem["nick"].is_null() || !mem.contains("nick")) {
        if (u.contains("global_name") && !u["global_name"].is_null())
          displayName = u["global_name"].get<std::string>();
        else if (u.contains("username"))
          displayName = u["username"].get<std::string>();
      }
      if (u.contains("avatar") && !u["avatar"].is_null())
        avatarHash = u["avatar"].get<std::string>();
      if (u.contains("username"))
        username = u["username"].get<std::string>();
    }
  } else if (d.contains("user") && !d["user"].is_null()) {
    const auto &u = d["user"];
    if (u.contains("global_name") && !u["global_name"].is_null())
      displayName = u["global_name"].get<std::string>();
    else if (u.contains("username"))
      displayName = u["username"].get<std::string>();
    if (u.contains("avatar") && !u["avatar"].is_null())
      avatarHash = u["avatar"].get<std::string>();
    if (u.contains("username"))
      username = u["username"].get<std::string>();
  }

  std::lock_guard<std::mutex> vl(m_VoiceMutex);
  if (channelId.empty()) {
    for (auto it = m_VoiceMembers.begin(); it != m_VoiceMembers.end(); ++it) {
      if (it->m_Id == userId) {
        m_VoiceMembers.erase(it);
        break;
      }
    }
  } else {
    bool found = false;
    for (auto &m : m_VoiceMembers) {
      if (m.m_Id == userId) {
        m.m_IsMuted = d.value("mute", false) || d.value("self_mute", false);
        m.m_IsDeafened = d.value("deaf", false) || d.value("self_deaf", false);
        m.m_ChannelId = channelId;
        if (!displayName.empty())
          m.m_DisplayName = displayName;
        if (!avatarHash.empty())
          m.m_AvatarHash = avatarHash;
        found = true;
        break;
      }
    }
    if (!found) {
      VoiceMember vm;
      vm.m_Id = userId;
      vm.m_Username =
          username.empty() ? ("User " + userId.substr(0, 4)) : username;
      vm.m_DisplayName = displayName;
      vm.m_AvatarHash = avatarHash;
      vm.m_ChannelId = channelId;
      vm.m_IsMuted = d.value("mute", false) || d.value("self_mute", false);
      vm.m_IsDeafened = d.value("deaf", false) || d.value("self_deaf", false);
      m_VoiceMembers.push_back(vm);
    }
  }
}
