#include "discord_client.h"
#include "vendor/nlohmann/json.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#define NOMINMAX
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#include <mmsystem.h>   // waveIn/waveOut, WAVEFORMATEX, WAVEHDR, etc.
#include "gui.h"
#include "vendor/include/opus.h"
#include "vendor/include/sodium.h"
#include "vendor/libdave/include/dave/dave.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "opus.lib")
#pragma comment(lib, "libsodium.lib")
#pragma comment(lib, "vendor/libdave/lib/libdave.lib")
#pragma comment(lib, "winmm.lib")

using json = nlohmann::json;

#include <fstream>
#include <mutex>
static std::mutex g_DebugMutex;
void DebugLog(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_DebugMutex);
    std::ofstream f("discord_debug.log", std::ios::app);
    f << text << "\n";
    std::cout << text << std::endl;
}

void DaveLogSink(DAVELoggingSeverity severity, const char* file, int line, const char* message) {
    std::string prefix = "[LIBDAVE] ";
    switch (severity) {
    case DAVE_LOGGING_SEVERITY_VERBOSE: prefix += "[V] "; break;
    case DAVE_LOGGING_SEVERITY_INFO:    prefix += "[I] "; break;
    case DAVE_LOGGING_SEVERITY_WARNING: prefix += "[W] "; break;
    case DAVE_LOGGING_SEVERITY_ERROR:   prefix += "[E] "; break;
    default: break;
    }
    DebugLog(prefix + std::string(message) + " (" + file + ":" + std::to_string(line) + ")");
}

void OnMlsFailure(const char* source, const char* reason, void* userData) {
    DebugLog(std::string("[LIBDAVE MLS FAILURE] ") + source + ": " + reason);
}

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

void DiscordClient::SetToken(const std::string& token) { m_Token = token; }
std::string DiscordClient::GetToken() const { return m_Token; }
std::string DiscordClient::GetSessionId() const {
    std::lock_guard<std::mutex> lock(const_cast<DiscordClient*>(this)->m_IdMutex);
    return m_SessionId;
}
std::string DiscordClient::GetUserId() const {
    std::lock_guard<std::mutex> lock(const_cast<DiscordClient*>(this)->m_IdMutex);
    return m_UserId;
}
std::string DiscordClient::GetUserName() const { return m_DisplayName; }
std::string DiscordClient::GetUserAvatar() const { return m_AvatarHash; }

bool DiscordClient::ValidateToken(const std::string& token) {
    DiscordClient temp;
    temp.SetToken(token);
    std::string resp = temp.HttpRequest("GET", "/api/v9/users/@me");
    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        if (j.contains("id") && j.contains("username")) return true;
    }
    catch (...) {}
    return false;
}

std::string DiscordClient::LoginWithCredentials(const std::string& email,
    const std::string& password,
    std::string& out_mfa_ticket) {
    DiscordClient temp;
    json payload = { {"login", email}, {"password", password},
                    {"undelete", false}, {"captcha_key", nullptr},
                    {"login_source", nullptr}, {"gift_code_sku_id", nullptr} };
    std::string resp = temp.HttpRequest("POST", "/api/v9/auth/login", payload.dump());
    if (resp.empty()) return "";
    try {
        auto j = json::parse(resp);
        if (j.contains("token") && !j["token"].is_null()) return j["token"].get<std::string>();
        if (j.contains("mfa") && j["mfa"].get<bool>() && j.contains("ticket")) {
            out_mfa_ticket = j["ticket"].get<std::string>();
            return "";
        }
    }
    catch (...) {}
    return "";
}

std::string DiscordClient::SubmitMfaCode(const std::string& code, const std::string& ticket) {
    DiscordClient temp;
    json payload = { {"code", code}, {"ticket", ticket} };
    std::string resp = temp.HttpRequest("POST", "/api/v9/auth/mfa/totp", payload.dump());
    if (resp.empty()) return "";
    try {
        auto j = json::parse(resp);
        if (j.contains("token") && !j["token"].is_null()) return j["token"].get<std::string>();
    }
    catch (...) {}
    return "";
}

bool DiscordClient::Connect() {
    if (m_Token.empty()) return false;
    if (m_Connected) return true;
    std::string resp = HttpRequest("GET", "/api/v9/users/@me");
    if (!resp.empty()) {
        try {
            auto j = json::parse(resp);
            if (j.contains("id") && j["id"].is_string()) m_UserId = j["id"].get<std::string>();
            if (j.contains("global_name") && !j["global_name"].is_null() && j["global_name"].is_string())
                m_DisplayName = j["global_name"].get<std::string>();
            else if (j.contains("username") && j["username"].is_string())
                m_DisplayName = j["username"].get<std::string>();
            if (j.contains("avatar") && !j["avatar"].is_null() && j["avatar"].is_string())
                m_AvatarHash = j["avatar"].get<std::string>();
        }
        catch (...) {}
    }
    m_Connected = true;
    m_WsThread = std::thread(&DiscordClient::WebSocketLoop, this);
    return true;
}

void DiscordClient::Disconnect() {
    m_Connected = false;
    m_RunHeartbeat = false;
    {
        std::lock_guard<std::mutex> lock(m_WsMutex);
        if (m_hWebSocket) {
            WinHttpCloseHandle((HINTERNET)m_hWebSocket);
            m_hWebSocket = nullptr;
        }
    }
    if (m_WsThread.joinable()) m_WsThread.detach();
    if (m_HeartbeatThread.joinable()) m_HeartbeatThread.detach();
}

void DiscordClient::SetOnMessageCallback(std::function<void(const DiscordMessage&)> cb) { m_MessageCallback = cb; }
void DiscordClient::SetOnConnectedCallback(std::function<void()> cb) { m_ConnectedCallback = cb; }
void DiscordClient::SetOnCallCallback(std::function<void(const std::string&, const std::string&)> cb) { m_CallCallback = cb; }
bool DiscordClient::IsConnected() const { return m_Connected; }

std::string DiscordClient::HttpRequest(const std::string& method, const std::string& path, const std::string& body) {
    DebugLog("[HTTP] " + method + " " + path);
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return "";
    HINTERNET connect = WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return ""; }
    std::wstring wPath(path.begin(), path.end());
    std::wstring wMethod(method.begin(), method.end());
    HINTERNET request = WinHttpOpenRequest(connect, wMethod.c_str(), wPath.c_str(), NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ""; }
    std::string authHeader = "Authorization: " + m_Token + "\r\n";
    std::string contentHeader = "Content-Type: application/json\r\n";
    std::string combinedHeaders = authHeader + contentHeader;
    std::wstring wheaders(combinedHeaders.begin(), combinedHeaders.end());
    BOOL sent = WinHttpSendRequest(request, wheaders.c_str(), -1, (LPVOID)body.c_str(),
        (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) { DebugLog("[HTTP] Request failed for " + path); WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ""; }
    WinHttpReceiveResponse(request, NULL);
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &statusCode, &size, NULL);
    DebugLog("[HTTP] Status: " + std::to_string(statusCode));
    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead))
            responseBody.append(buffer.data(), bytesRead);
    }
    DebugLog("[HTTP] Response length: " + std::to_string(responseBody.size()));
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return responseBody;
}

std::vector<DiscordGuild> DiscordClient::FetchGuilds() {
    std::vector<DiscordGuild> guilds;
    std::string resp = HttpRequest("GET", "/api/v9/users/@me/guilds");
    if (resp.empty()) return guilds;
    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (const auto& item : j) {
                if (!item.contains("id") || !item["id"].is_string()) continue;
                DiscordGuild g;
                g.id = item["id"].get<std::string>();
                g.name = item.value("name", "Unknown Guild");
                if (item.contains("icon") && !item["icon"].is_null() && item["icon"].is_string())
                    g.icon_hash = item["icon"].get<std::string>();
                guilds.push_back(g);
            }
        }
    }
    catch (...) {}
    return guilds;
}

std::vector<DiscordChannel> DiscordClient::FetchChannels(const std::string& guild_id) {
    std::vector<DiscordChannel> channels;
    std::string resp = HttpRequest("GET", "/api/v9/guilds/" + guild_id + "/channels");
    if (resp.empty()) return channels;
    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            std::map<std::string, bool> category_locked;
            auto isNodeLocked = [&](const json& item) -> bool {
                if (item.contains("permission_overwrites") && item["permission_overwrites"].is_array()) {
                    for (const auto& ow : item["permission_overwrites"]) {
                        if (ow.contains("id") && ow["id"].is_string() && ow["id"] == guild_id) {
                            if (ow.contains("deny")) {
                                std::string deny_str = ow["deny"].is_string() ? ow["deny"].get<std::string>()
                                    : std::to_string(ow["deny"].get<long long>());
                                if (!deny_str.empty()) {
                                    unsigned long long deny_bits = std::stoull(deny_str);
                                    if ((deny_bits & 1024ULL) != 0) return true;
                                }
                            }
                        }
                    }
                }
                return false;
                };
            for (const auto& item : j) {
                if (item.contains("type") && item["type"].is_number() && item["type"].get<int>() == 4) {
                    if (item.contains("id") && item["id"].is_string())
                        category_locked[item["id"].get<std::string>()] = isNodeLocked(item);
                }
            }
            for (const auto& item : j) {
                if (!item.contains("type") || !item["type"].is_number()) continue;
                int type = item["type"].get<int>();
                if (type == 0 || type == 2) {
                    bool locked = isNodeLocked(item);
                    if (!locked && item.contains("parent_id") && !item["parent_id"].is_null()) {
                        std::string parent_id = item["parent_id"].get<std::string>();
                        if (category_locked[parent_id]) locked = true;
                    }
                    DiscordChannel ch;
                    ch.id = item.value("id", "");
                    ch.name = item.value("name", "unknown-channel");
                    ch.type = type;
                    ch.is_locked = locked;
                    ch.position = item.value("position", 0);
                    ch.parent_id = (item.contains("parent_id") && !item["parent_id"].is_null() && item["parent_id"].is_string())
                        ? item["parent_id"].get<std::string>() : "";
                    if (!ch.id.empty()) channels.push_back(ch);
                }
            }
        }
    }
    catch (...) {}
    return channels;
}

std::vector<DiscordChannel> DiscordClient::FetchPrivateChannels() {
    std::vector<DiscordChannel> channels;
    std::string resp = HttpRequest("GET", "/api/v9/users/@me/channels");
    if (resp.empty()) return channels;
    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (const auto& item : j) {
                std::string name = "";
                if (item.contains("name") && !item["name"].is_null()) {
                    name = item["name"].get<std::string>();
                }
                else if (item.contains("recipients") && item["recipients"].is_array() && !item["recipients"].empty()) {
                    for (size_t i = 0; i < item["recipients"].size(); ++i) {
                        if (i > 0) name += ", ";
                        const auto& rec = item["recipients"][i];
                        if (rec.contains("global_name") && !rec["global_name"].is_null())
                            name += rec["global_name"].get<std::string>();
                        else
                            name += rec["username"].get<std::string>();
                    }
                }
                if (name.empty()) name = "Unnamed DM";
                channels.push_back({ item["id"].get<std::string>(), name, item["type"].get<int>(), false });
            }
        }
    }
    catch (...) {}
    return channels;
}

std::vector<DiscordMessage> DiscordClient::FetchMessages(const std::string& channel_id, const std::string& before_id) {
    std::vector<DiscordMessage> msgs;
    std::string endpoint = "/api/v9/channels/" + channel_id + "/messages?limit=50";
    if (!before_id.empty()) endpoint += "&before=" + before_id;
    std::string resp = HttpRequest("GET", endpoint);
    if (resp.empty()) return msgs;
    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (auto it = j.rbegin(); it != j.rend(); ++it) {
                DiscordMessage dmsg;
                ParseJsonMessage(*it, dmsg);
                msgs.push_back(dmsg);
            }
        }
    }
    catch (...) {}
    return msgs;
}

bool DiscordClient::SendDiscordMessage(const std::string& channel_id, const std::string& content) {
    json payload = { {"content", content} };
    std::string resp = HttpRequest("POST", "/api/v9/channels/" + channel_id + "/messages", payload.dump());
    return !resp.empty();
}
bool DiscordClient::SendReply(const std::string& channel_id, const std::string& content, const std::string& reply_to_id) {
    json payload = { {"content", content}, {"message_reference", {{"message_id", reply_to_id}}} };
    std::string resp = HttpRequest("POST", "/api/v9/channels/" + channel_id + "/messages", payload.dump());
    return !resp.empty();
}
bool DiscordClient::EditMessage(const std::string& channel_id, const std::string& msg_id, const std::string& new_content) {
    json payload = { {"content", new_content} };
    std::string resp = HttpRequest("PATCH", "/api/v9/channels/" + channel_id + "/messages/" + msg_id, payload.dump());
    return !resp.empty();
}
bool DiscordClient::DeleteMessage(const std::string& channel_id, const std::string& msg_id) {
    std::string resp = HttpRequest("DELETE", "/api/v9/channels/" + channel_id + "/messages/" + msg_id);
    return !resp.empty();
}
bool DiscordClient::AddReaction(const std::string& channel_id, const std::string& msg_id, const std::string& emoji) {
    std::string path = "/api/v9/channels/" + channel_id + "/messages/" + msg_id + "/reactions/" + emoji + "/@me";
    HttpRequest("PUT", path, "");
    return true;
}

std::vector<unsigned char> DiscordClient::DownloadFile(const std::string& urlStr) {
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
    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp)) return data;
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<unsigned char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead))
                    data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
            }
        }
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return data;
}

bool DiscordClient::SendAttachment(const std::string& channel_id, const std::string& filepath) {
    if (m_Token.empty()) return false;
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> fileBuffer(size);
    if (!file.read(fileBuffer.data(), size)) return false;
    std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"payload_json\"\r\n\r\n";
    body += "{\"content\":\"\"}\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"files[0]\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    std::vector<char> requestBuffer(body.begin(), body.end());
    requestBuffer.insert(requestBuffer.end(), fileBuffer.begin(), fileBuffer.end());
    std::string endBoundary = "\r\n--" + boundary + "--\r\n";
    requestBuffer.insert(requestBuffer.end(), endBoundary.begin(), endBoundary.end());
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::string path = "/api/v9/channels/" + channel_id + "/messages";
    std::wstring wPath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(), NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string headers = "Authorization: " + m_Token + "\r\n";
    headers += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
    std::wstring wheaders(headers.begin(), headers.end());
    BOOL sent = WinHttpSendRequest(hRequest, wheaders.c_str(), -1, requestBuffer.data(),
        (DWORD)requestBuffer.size(), (DWORD)requestBuffer.size(), 0);
    if (sent) WinHttpReceiveResponse(hRequest, NULL);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return sent == TRUE;
}

void DiscordClient::ParseJsonMessage(const json& item, DiscordMessage& dmsg) {
    if (item.contains("id") && item["id"].is_string()) dmsg.id = item["id"].get<std::string>();
    if (item.contains("author") && !item["author"].is_null()) {
        const auto& au = item["author"];
        if (au.contains("username") && au["username"].is_string()) dmsg.author_username = au["username"].get<std::string>();
        if (au.contains("id") && au["id"].is_string()) dmsg.author_id = au["id"].get<std::string>();
        if (au.contains("avatar") && !au["avatar"].is_null() && au["avatar"].is_string()) dmsg.author_avatar = au["avatar"].get<std::string>();
        if (au.contains("global_name") && !au["global_name"].is_null() && au["global_name"].is_string())
            dmsg.author = au["global_name"].get<std::string>();
        else
            dmsg.author = dmsg.author_username;
    }
    if (item.contains("timestamp")) dmsg.timestamp = item["timestamp"].get<std::string>();
    if (item.contains("content")) dmsg.content = item["content"].get<std::string>();
    if (item.contains("attachments") && item["attachments"].is_array()) {
        for (const auto& att : item["attachments"]) {
            if (att.contains("url")) {
                std::string url = att["url"].get<std::string>();
                std::string fname = att.contains("filename") ? att["filename"].get<std::string>() : "";
                std::string ext = fname.size() >= 4 ? fname.substr(fname.size() - 4) : "";
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".png" || ext == ".jpg" || ext == "jpeg" || ext == ".gif" || ext == ".webp")
                    dmsg.attachment_urls.push_back(url);
                else if (ext == ".mp4" || ext == ".mov" || ext == ".webm" || ext == ".m4v")
                    dmsg.video_urls.push_back(url);
            }
        }
    }
    if (dmsg.attachment_urls.empty() && item.contains("embeds") && item["embeds"].is_array()) {
        for (const auto& emb : item["embeds"]) {
            if (emb.contains("image") && emb["image"].contains("url"))
                dmsg.attachment_urls.push_back(emb["image"]["url"].get<std::string>());
            else if (emb.contains("thumbnail") && emb["thumbnail"].contains("url"))
                dmsg.attachment_urls.push_back(emb["thumbnail"]["url"].get<std::string>());
        }
    }
    if (item.contains("reactions") && item["reactions"].is_array()) {
        for (const auto& r : item["reactions"]) {
            DiscordReaction dr;
            if (r.contains("emoji") && r["emoji"].contains("name")) dr.emoji = r["emoji"]["name"].get<std::string>();
            dr.count = r.contains("count") ? r["count"].get<int>() : 1;
            dr.me = r.contains("me") ? r["me"].get<bool>() : false;
            dmsg.reactions.push_back(dr);
        }
    }
    if (item.contains("referenced_message") && !item["referenced_message"].is_null()) {
        const auto& ref = item["referenced_message"];
        if (ref.contains("id") && ref["id"].is_string()) dmsg.referenced_message_id = ref["id"].get<std::string>();
        if (ref.contains("content") && ref["content"].is_string()) dmsg.referenced_content = ref["content"].get<std::string>();
        if (ref.contains("author") && !ref["author"].is_null()) {
            const auto& rau = ref["author"];
            if (rau.contains("global_name") && !rau["global_name"].is_null() && rau["global_name"].is_string())
                dmsg.referenced_author = rau["global_name"].get<std::string>();
            else if (rau.contains("username") && rau["username"].is_string())
                dmsg.referenced_author = rau["username"].get<std::string>();
        }
    }
}

void DiscordClient::SubscribeToGuild(const std::string& guildId) {
    if (guildId.empty()) return;
    DebugLog("[GATEWAY] Subscribing to guild: " + guildId);
    json sub = { {"op", 14}, {"d", {{"guild_id", guildId}, {"typing", true}, {"threads", true},
        {"activities", true}, {"members", json::array()}, {"channels", json::object()}}} };
    QueueWsMessage(sub.dump());
}

void DiscordClient::SendIdentify(void* hWebSocket) {
    json identify = { {"op", 2}, {"d", {{"token", m_Token}, {"properties",
        {{"os", "Windows"}, {"browser", "Discord Client"}, {"release_channel", "stable"},
        {"client_version", "1.0.9219"}, {"os_version", "10.0.19045"}, {"os_arch", "x64"},
        {"system_locale", "en-US"}, {"client_build_number", 362175},
        {"native_build_number", 59134}, {"client_event_source", nullptr}}},
        {"compress", false}, {"client_state", {{"capabilities", 16383},
        {"highest_last_message_id", "0"}, {"read_state_version", 0},
        {"user_guild_settings_version", -1}, {"user_settings_version", -1}}}}} };
    QueueWsMessage(identify.dump());
}

void DiscordClient::WebSocketLoop() {
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, L"gateway.discord.gg", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/?v=10&encoding=json", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
    LPCWSTR headers = L"Origin: https://discord.com\r\n"
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n";
    WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
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
    const DWORD cbBuffer = 65536;
    BYTE* pbBuffer = new BYTE[cbBuffer];
    std::string accumulator;
    while (m_Connected) {
        DWORD cbDataRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType;
        DWORD dwError = WinHttpWebSocketReceive(hWebSocket, pbBuffer, cbBuffer, &cbDataRead, &eBufferType);
        if (dwError != ERROR_SUCCESS) break;
        if (eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        accumulator.append((char*)pbBuffer, cbDataRead);
        if (eBufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::string msg = accumulator;
            accumulator.clear();
            try {
                auto j = json::parse(msg);
                int op = j["op"].get<int>();
                if (j.contains("s") && !j["s"].is_null()) m_SequenceNumber = j["s"].get<int>();
                if (op == 10) {
                    m_HeartbeatInterval = j["d"]["heartbeat_interval"].get<int>();
                    m_RunHeartbeat = true;
                    m_HeartbeatThread = std::thread(&DiscordClient::HeartbeatLoop, this);
                    SendIdentify(hWebSocket);
                }
                else if (op == 0) {
                    std::string t = j["t"].get<std::string>();
                    if (t == "READY") {
                        DebugLog("[GATEWAY] Received READY event (Assembled). Deep-scanning...");
                        {
                            std::lock_guard<std::mutex> lock(m_IdMutex);
                            if (j.contains("d")) {
                                const auto& d = j["d"];
                                if (d.contains("session_id")) m_SessionId = d["session_id"].get<std::string>();
                                if (m_SessionId.empty() && d.contains("sessions") && d["sessions"].is_array() && !d["sessions"].empty()) {
                                    if (d["sessions"][0].contains("session_id"))
                                        m_SessionId = d["sessions"][0]["session_id"].get<std::string>();
                                }
                                if (d.contains("user") && d["user"].contains("id")) m_UserId = d["user"]["id"].get<std::string>();
                            }
                        }
                        if (!m_SessionId.empty()) DebugLog("[GATEWAY] SUCCESS: Captured Primary Session ID: " + m_SessionId);
                        else DebugLog("[GATEWAY] ERROR: No session ID found in assembled READY payload.");
                        if (m_ConnectedCallback) m_ConnectedCallback();
                        json presence = { {"op", 3}, {"d", {{"status", "online"}, {"since", 0}, {"activities", json::array()}, {"afk", false}}} };
                        QueueWsMessage(presence.dump());
                    }
                    else if (t == "MESSAGE_CREATE") {
                        if (m_MessageCallback) {
                            DiscordMessage dmsg;
                            ParseJsonMessage(j["d"], dmsg);
                            m_MessageCallback(dmsg);
                        }
                    }
                    else if (t == "GUILD_CREATE") {
                        if (j["d"].contains("voice_states") && j["d"]["voice_states"].is_array()) {
                            for (const auto& vs : j["d"]["voice_states"]) ParseVoiceStateUpdate(vs);
                        }
                    }
                    else if (t == "VOICE_STATE_UPDATE") {
                        std::string channelId = (j["d"].contains("channel_id") && !j["d"]["channel_id"].is_null())
                            ? j["d"]["channel_id"].get<std::string>() : "";
                        if (j["d"].contains("user_id") && j["d"]["user_id"].is_string() &&
                            j["d"]["user_id"].get<std::string>() == m_UserId) {
                            if (j["d"].contains("session_id") && !j["d"]["session_id"].is_null()) {
                                m_VoiceSessionId = j["d"]["session_id"].get<std::string>();
                                DebugLog("[GATEWAY] Captured OUR Voice Session ID: " + m_VoiceSessionId);
                            }
                            m_VoiceConn.m_ChannelId = channelId;
                        }
                        ParseVoiceStateUpdate(j["d"]);
                        if (!channelId.empty()) {
                            std::lock_guard<std::mutex> lock(m_WsMutex);
                            if (!m_VoiceConn.m_Token.empty() && !m_VoiceConn.m_Endpoint.empty() &&
                                !m_VoiceSessionId.empty() && !m_VoiceConn.m_Running) {
                                m_VoiceConn.m_Running = true;
                                if (m_VoiceConn.m_VoiceThread.joinable()) m_VoiceConn.m_VoiceThread.detach();
                                m_VoiceConn.m_VoiceThread = std::thread(&DiscordClient::VoiceLoop, this,
                                    m_VoiceConn.m_Endpoint, m_VoiceConn.m_Token, m_VoiceConn.m_GuildId, m_VoiceSessionId, m_UserId);
                            }
                        }
                    }
                    else if (t == "VOICE_SERVER_UPDATE") {
                        if (j["d"].contains("endpoint") && !j["d"]["endpoint"].is_null()) {
                            m_VoiceConn.m_Endpoint = j["d"]["endpoint"].get<std::string>();
                            m_VoiceConn.m_Token = j["d"]["token"].get<std::string>();
                            m_VoiceConn.m_GuildId = (j["d"].contains("guild_id") && !j["d"]["guild_id"].is_null())
                                ? j["d"]["guild_id"].get<std::string>() : "";
                            DebugLog("[GATEWAY] VOICE_SERVER_UPDATE: Token=" + m_VoiceConn.m_Token);
                            if (!m_VoiceConn.m_ChannelId.empty()) {
                                std::lock_guard<std::mutex> lock(m_WsMutex);
                                if (!m_VoiceSessionId.empty() && !m_VoiceConn.m_Running) {
                                    m_VoiceConn.m_Running = true;
                                    if (m_VoiceConn.m_VoiceThread.joinable()) m_VoiceConn.m_VoiceThread.detach();
                                    m_VoiceConn.m_VoiceThread = std::thread(&DiscordClient::VoiceLoop, this,
                                        m_VoiceConn.m_Endpoint, m_VoiceConn.m_Token, m_VoiceConn.m_GuildId, m_VoiceSessionId, m_UserId);
                                }
                            }
                        }
                    }
                    else if (t == "CALL_CREATE" || t == "CALL_UPDATE") {
                        DebugLog("[GATEWAY] CALL EVENT (" + t + "): " + j["d"].dump());
                        if (m_CallCallback && j["d"].contains("channel_id")) {
                            std::string cid = j["d"]["channel_id"].get<std::string>();
                            bool weAreCalling = false;
                            if (j["d"].contains("ongoing_rings") && j["d"]["ongoing_rings"].is_object()) {
                                for (auto& it : j["d"]["ongoing_rings"].items()) {
                                    if (it.value().is_string() && it.value().get<std::string>() == m_UserId) {
                                        weAreCalling = true; break;
                                    }
                                }
                            }
                            if (weAreCalling) DebugLog("[GATEWAY] We are the caller. Ignoring overlay.");
                            else m_CallCallback(cid, "Incoming Call...");
                        }
                    }
                    else if (t == "CALL_DELETE") {
                        DebugLog("[GATEWAY] CALL TERMINATED. Clearing overlay.");
                        if (m_CallCallback) m_CallCallback("", "STOP");
                    }
                }
            }
            catch (const std::exception& e) {
                DebugLog("[GATEWAY] Parse Error: " + std::string(e.what()));
            }
        }
    }
    delete[] pbBuffer;
    {
        std::lock_guard<std::mutex> lock(m_WsMutex);
        if (m_hWebSocket) {
            WinHttpWebSocketClose((HINTERNET)m_hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle((HINTERNET)m_hWebSocket);
            m_hWebSocket = nullptr;
        }
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    m_Connected = false;
}

bool DiscordClient::JoinVoiceChannel(const std::string& guild_id, const std::string& channel_id) {
    DebugLog("[VOICE-JOIN] Entering JoinVoiceChannel...");
    std::lock_guard<std::mutex> lock(m_WsMutex);
    DebugLog("[VOICE-JOIN] Mutex locked. Resetting state...");
    m_VoiceReady = false;
    m_VoiceSessionId.clear();
    m_VoiceConn.m_Running = false;
    m_VoiceConn.m_GuildId = guild_id;
    m_VoiceConn.m_ChannelId = channel_id;
    m_VoiceConn.m_Token.clear();
    m_VoiceConn.m_Endpoint.clear();
    m_VoiceConn.m_Ready = false;
    m_VoiceConn.m_DaveHandshakeComplete = false;
    DebugLog("[VOICE-JOIN] Preparing JSON...");
    json d;
    if (!guild_id.empty()) d["guild_id"] = guild_id;
    if (channel_id.empty()) d["channel_id"] = nullptr;
    else d["channel_id"] = channel_id;
    d["self_mute"] = false;
    d["self_deaf"] = false;
    d["self_video"] = false;
    json j;
    j["op"] = 4;
    j["d"] = d;
    DebugLog("[VOICE-JOIN] Sending Op 4...");
    QueueWsMessage(j.dump());
    DebugLog("[VOICE-JOIN] Exit.");
    return true;
}

void DiscordClient::LeaveVoiceChannel(const std::string& guild_id) {
    JoinVoiceChannel(guild_id, "");
    {
        std::lock_guard<std::mutex> lock(m_WsMutex);
        if (m_VoiceConn.m_hVoiceWS) {
            WinHttpWebSocketClose((HINTERNET)m_VoiceConn.m_hVoiceWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle((HINTERNET)m_VoiceConn.m_hVoiceWS);
            m_VoiceConn.m_hVoiceWS = nullptr;
        }
    }
    m_VoiceConn.m_Running = false;
}

void DiscordClient::SetVoiceState(bool muted, bool deafened) {
    m_VoiceConn.m_IsMuted = muted;
    m_VoiceConn.m_IsDeafened = deafened;
}

void DiscordClient::SetAudioDevices(int inputIdx, int outputIdx) {
    m_VoiceConn.m_InputDevice = inputIdx;
    m_VoiceConn.m_OutputDevice = outputIdx;
}

std::vector<VoiceMember> DiscordClient::GetVoiceMembers(const std::string& channel_id) {
    std::lock_guard<std::mutex> lock(m_VoiceMutex);
    std::vector<VoiceMember> filtered;
    for (const auto& m : m_VoiceMembers) {
        if (m.m_ChannelId == channel_id) filtered.push_back(m);
    }
    return filtered;
}

void DiscordClient::VoiceLoop(std::string endpoint, std::string token,
    std::string guildId, std::string sessionId,
    std::string userId) {
    DebugLog("[VOICE] Starting VoiceLoop with endpoint: " + endpoint);
    int port = 443;
    if (endpoint.find("wss://") == 0) endpoint = endpoint.substr(6);
    size_t colon = endpoint.find(':');
    if (colon != std::string::npos) {
        port = std::stoi(endpoint.substr(colon + 1));
        endpoint = endpoint.substr(0, colon);
    }
    DebugLog("[VOICE] Cleaned endpoint: " + endpoint + " port: " + std::to_string(port));
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    HINTERNET hConnect = WinHttpConnect(hSession, std::wstring(endpoint.begin(), endpoint.end()).c_str(),
        (INTERNET_PORT)port, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/?v=8", NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
    DWORD secureProtocols = 0x00000800 | 0x00002000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
    std::string hostHeader = "Host: " + endpoint + ":" + std::to_string(port) + "\r\n";
    std::wstring wHeaders(L"Origin: https://discord.com\r\n"
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9150 Chrome/121.0.6167.184 Electron/29.1.0 Safari/537.36\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n");
    wHeaders += std::wstring(hostHeader.begin(), hostHeader.end());
    if (!WinHttpSendRequest(hRequest, wHeaders.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        DebugLog("[VOICE] WinHttpSendRequest or ReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }
    HINTERNET hVoiceWS = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
    WinHttpCloseHandle(hRequest);
    if (!hVoiceWS) {
        DebugLog("[VOICE] WebSocket Upgrade failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_WsMutex);
        m_VoiceConn.m_hVoiceWS = (HINTERNET)hVoiceWS;
    }
    DebugLog("[VOICE] WebSocket connected, waiting for Hello...");
    SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    int timeout = 2000;
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    m_VoiceConn.m_UdpSocket = udpSocket;
    m_VoiceConn.m_TxCounter = 0;
    m_VoiceConn.m_PlaybackThread = std::thread(&DiscordClient::AudioPlaybackLoop, this);
    daveSetLogSinkCallback(DaveLogSink);
    m_VoiceConn.m_DaveVersion = daveMaxSupportedProtocolVersion();
    m_VoiceConn.m_DaveSession = daveSessionCreate(nullptr, nullptr, OnMlsFailure, nullptr);
    DebugLog("[VOICE] Dave Protocol Version: " + std::to_string(m_VoiceConn.m_DaveVersion));
    m_VoiceConn.m_RecognizedUserIds.clear();
    m_VoiceConn.m_RecognizedUserIds.push_back(userId);
    const DWORD cbBuffer = 8192;
    BYTE* pbBuffer = new BYTE[cbBuffer];
    while (m_VoiceConn.m_Running) {
        DWORD cbRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        std::string wsMessage;
        do {
            if (WinHttpWebSocketReceive(hVoiceWS, pbBuffer, cbBuffer, &cbRead, &type) != ERROR_SUCCESS) {
                DebugLog("[VOICE] WS Receive Error");
                break;
            }
            wsMessage.append((char*)pbBuffer, cbRead);
        } while (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
        if (wsMessage.empty()) {
            USHORT code = 0;
            BYTE reason[128];
            DWORD reasonLen = 0;
            WinHttpWebSocketQueryCloseStatus(hVoiceWS, &code, reason, sizeof(reason), &reasonLen);
            DebugLog("[VOICE] Connection closed by server. Code: " + std::to_string(code));
            if (code == 4005 || code == 4006) {
                // 4005 = Already Authenticated, 4006 = Session no longer valid.
                // Do NOT auto-rejoin - clear running flag so GUI knows we're disconnected.
                DebugLog("[VOICE] " + std::to_string(code) + ": Session invalid - clearing voice state. Please try joining again.");
                m_VoiceConn.m_Running = false;
                // Signal gateway to clear our voice state so Discord cleans up
                json leave = {{"op", 4}, {"d", {
                    {"guild_id", m_VoiceConn.m_GuildId.empty() ? nullptr : json(m_VoiceConn.m_GuildId)},
                    {"channel_id", nullptr},
                    {"self_mute", false},
                    {"self_deaf", false}
                }}};
                QueueWsMessage(leave.dump());
                // Wait for Discord to fully clear session before allowing reconnect
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            }
            break;
        }
        try {
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                if (wsMessage.size() < 3) continue;
                uint16_t seq = (uint8_t(wsMessage[0]) << 8) | uint8_t(wsMessage[1]);
                uint8_t op = uint8_t(wsMessage[2]);
                const uint8_t* payload = (const uint8_t*)wsMessage.data() + 3;
                size_t payloadLen = wsMessage.size() - 3;
                
                DebugLog("[VOICE BIN RX] Op: " + std::to_string(op) + " Seq: " + std::to_string(seq) + " Len: " + std::to_string(payloadLen));
                
                // Op 25: External Sender (DAVE MLS external sender credentials)
                if (op == 25) {
                    DebugLog("[VOICE] DAVE External Sender received, len=" + std::to_string(payloadLen));
                    if (m_VoiceConn.m_DaveSession && payloadLen > 0) {
                        daveSessionSetExternalSender(
                            (DAVESessionHandle)m_VoiceConn.m_DaveSession, 
                            payload, payloadLen
                        );
                        DebugLog("[VOICE] daveSessionSetExternalSender OK - pending group created, waiting for Op 27 (Proposals) from server");
                        // After setExternalSender, libdave has created a pending group.
                        // The server will now send Op 27 (proposals for us to commit) or Op 30 (welcome into existing group).
                        // We must NOT call processProposals yet - wait for the server's Op 27 payload.
                    }
                }

                
                // Op 30: MLS Welcome (joining existing call - we are a new joiner)
                else if (op == 30) {
                    // Op 30 format: [2-byte transition_id][welcome_bytes...]
                    if (payloadLen < 2) {
                        DebugLog("[VOICE] Op 30 too short: " + std::to_string(payloadLen));
                    } else {
                        uint16_t tid = (payload[0] << 8) | payload[1];
                        DebugLog("[VOICE] MLS Welcome (Op 30), transition=" + std::to_string(tid) + " len=" + std::to_string(payloadLen - 2));
                        
                        if (m_VoiceConn.m_DaveSession && payloadLen > 2) {
                            std::vector<const char*> users;
                            for (auto& u : m_VoiceConn.m_RecognizedUserIds) 
                                users.push_back(u.c_str());
                            
                            void* result = daveSessionProcessWelcome(
                                (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                                payload + 2, payloadLen - 2, users.data(), users.size()
                            );
                            
                            if (result) {
                                DebugLog("[VOICE] Welcome processed! Sending Transition Ready (Op 23), tid=" + std::to_string(tid));
                                
                                // Send Transition Ready with server's transition ID as JSON
                                json jReady = { {"op", 23}, {"d", {{"transition_id", tid}}} };
                                std::string sReady = jReady.dump();
                                { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                                  WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sReady.c_str(), (DWORD)sReady.size()); }
                                DebugLog("[VOICE] Sent Transition Ready (Op 23) tid=" + std::to_string(tid) + " as JSON");
                                
                                UpdateDaveKeys();
                                daveWelcomeResultDestroy((DAVEWelcomeResultHandle)result);
                            } else {
                                DebugLog("[VOICE] Welcome processing FAILED");
                            }
                        }
                    }
                }
                
                // Op 27: Proposals (server sends add-proposals to the coordinator)
                // Format: [2-byte transition_id][proposals_bytes...]
                else if (op == 27) {
                    if (payloadLen < 2) {
                        DebugLog("[VOICE] Op 27 too short: " + std::to_string(payloadLen));
                    } else {
                        uint16_t tid = (payload[0] << 8) | payload[1];
                        const uint8_t* proposalData = payload + 2;
                        size_t proposalLen = payloadLen - 2;
                        
                        std::string hexDump;
                        size_t dumpLen = (payloadLen < 48) ? payloadLen : 48;
                        char hbuf[4];
                        for (size_t i = 0; i < dumpLen; i++) {
                            snprintf(hbuf, sizeof(hbuf), "%02x", payload[i]);
                            hexDump += hbuf;
                        }
                        DebugLog("[VOICE] Proposals (Op 27) tid=" + std::to_string(tid) + " proposalLen=" + std::to_string(proposalLen) + " hex=" + hexDump);
                        
                        if (m_VoiceConn.m_DaveSession && proposalLen > 0) {
                            std::vector<const char*> users;
                            for (auto& u : m_VoiceConn.m_RecognizedUserIds)
                                users.push_back(u.c_str());
                            
                            uint8_t* cw = nullptr;
                            size_t cwLen = 0;
                            daveSessionProcessProposals(
                                (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                                proposalData, proposalLen, users.data(), users.size(),
                                &cw, &cwLen
                            );
                            
                            if (cw && cwLen > 0) {
                                // Op 28: [op=28][tid_hi][tid_lo][commit+welcome_bytes]
                                std::vector<uint8_t> msg = { 28, (uint8_t)(tid >> 8), (uint8_t)(tid & 0xFF) };
                                msg.insert(msg.end(), cw, cw + cwLen);
                                { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                                  WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                    msg.data(), (DWORD)msg.size()); }
                                DebugLog("[VOICE] Sent Commit/Welcome (Op 28) tid=" + std::to_string(tid) + " bytes=" + std::to_string(cwLen));
                                daveFree(cw);
                                
                                UpdateDaveKeys();
                            } else {
                                DebugLog("[VOICE] WARNING: processProposals returned no commit for Op 27!");
                            }
                        } else if (proposalLen == 0) {
                            DebugLog("[VOICE] Op 27 has empty proposal bytes after tid");
                        }
                    }
                }
                
                // Op 29: Commit
                else if (op == 29 && payloadLen >= 2) {
                    uint16_t tid = (payload[0] << 8) | payload[1];
                    DebugLog("[VOICE] Commit (Op 29) transition=" + std::to_string(tid));
                    
                    if (m_VoiceConn.m_DaveSession) {
                        void* result = daveSessionProcessCommit(
                            (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                            payload + 2, payloadLen - 2
                        );
                        if (result) {
                            json jReady = { {"op", 23}, {"d", {{"transition_id", tid}}} };
                            std::string sReady = jReady.dump();
                            { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                              WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sReady.c_str(), (DWORD)sReady.size()); }
                            DebugLog("[VOICE] Sent Op 23 for transition " + std::to_string(tid) + " as JSON");
                            
                            UpdateDaveKeys();
                            
                            daveCommitResultDestroy((DAVECommitResultHandle)result);
                        }
                    }
                }
                
                // Op 22: Execute Transition
                else if (op == 22) {
                    DebugLog("[VOICE] 🎉 EXECUTE TRANSITION - DAVE READY!");
                    m_VoiceConn.m_DaveHandshakeComplete = true;
                    m_VoiceConn.m_Ready = true;
                    m_VoiceReady = true;
                    
                    // Start audio thread
                    std::thread(&DiscordClient::AudioCaptureLoop, this).detach();
                }
                
                // Log any unhandled binary opcode so we never miss server messages
                else {
                    // Hex-dump first 32 bytes for inspection
                    std::string hexDump;
                    size_t dumpLen = (payloadLen < 32) ? payloadLen : 32;
                    char buf[4];
                    for (size_t i = 0; i < dumpLen; i++) {
                        snprintf(buf, sizeof(buf), "%02x", payload[i]);
                        hexDump += buf;
                    }
                    DebugLog("[VOICE BIN UNKNOWN] Op=" + std::to_string(op) + " Seq=" + std::to_string(seq) + " Len=" + std::to_string(payloadLen) + " Data=" + hexDump);
                }
                
                continue;
            }
            DebugLog("[VOICE RX RAW] " + wsMessage);
            auto j = json::parse(wsMessage);
            int op = j["op"];
            auto d = j["d"];
            if (j.contains("seq") && !j["seq"].is_null()) {
                m_VoiceConn.m_VoiceSeqAck = j["seq"].get<int>();
            }
            if (op == 2) {
                DebugLog("[VOICE] AUTHENTICATION SUCCESS! Received READY (Op 2)");
                OutputDebugStringA("[VOICE] Received READY (Op 2)\n");
                uint32_t ssrc = d["ssrc"].get<uint32_t>();
                m_VoiceConn.m_Ssrc = ssrc;
                std::string ip = d["ip"].get<std::string>();
                int port = d["port"].get<int>();
                struct addrinfo hints = {}, * res = nullptr;
                hints.ai_family = AF_INET;
                if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
                    m_VoiceConn.m_ServerAddr = *(struct sockaddr_in*)res->ai_addr;
                    freeaddrinfo(res);
                }
                else {
                    m_VoiceConn.m_ServerAddr = {};
                    m_VoiceConn.m_ServerAddr.sin_family = AF_INET;
                    m_VoiceConn.m_ServerAddr.sin_port = htons((u_short)port);
                    inet_pton(AF_INET, ip.c_str(), &m_VoiceConn.m_ServerAddr.sin_addr);
                }

                // Perform UDP IP-discovery synchronously on this thread.
                // Discord validates the address in Op 1 (Select Protocol) and rejects
                // its own server IP — we MUST send our real external IP/port.
                // Max wait: 3 x 100ms = 300ms, well within any server timeout.
                std::string myIp = ip;   // fallback: server IP
                int myPort = port;
                {
                    int disc_timeout = 100;
                    setsockopt(m_VoiceConn.m_UdpSocket, SOL_SOCKET, SO_RCVTIMEO,
                        (char*)&disc_timeout, sizeof(disc_timeout));

                    unsigned char disc_pkt[74] = { 0 };
                    *(uint16_t*)(disc_pkt)     = htons(1);
                    *(uint16_t*)(disc_pkt + 2) = htons(70);
                    *(uint32_t*)(disc_pkt + 4) = htonl(ssrc);

                    for (int retry = 0; retry < 3; ++retry) {
                        sendto(m_VoiceConn.m_UdpSocket, (char*)disc_pkt, 74, 0,
                            (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
                        char resp[74] = { 0 };
                        struct sockaddr_in from{};
                        int fromLen = sizeof(from);
                        if (recvfrom(m_VoiceConn.m_UdpSocket, resp, 74, 0,
                            (struct sockaddr*)&from, &fromLen) > 0) {
                            char szIp[64] = { 0 };
                            for (int k = 0; k < 63 && resp[8 + k]; k++) szIp[k] = resp[8 + k];
                            myIp   = szIp;
                            myPort = (int)ntohs(*(uint16_t*)(resp + 72));
                            DebugLog("[VOICE] UDP Discovery OK: external ip=" + myIp + " port=" + std::to_string(myPort));
                            break;
                        }
                    }
                    if (myIp == ip)
                        DebugLog("[VOICE] UDP Discovery failed after 300ms, falling back to server ip=" + ip);

                    // Restore socket timeout for UdpReceiveLoop (started later on Op 4)
                    int normal_timeout = 2000;
                    setsockopt(m_VoiceConn.m_UdpSocket, SOL_SOCKET, SO_RCVTIMEO,
                        (char*)&normal_timeout, sizeof(normal_timeout));
                }

                // Send Select Protocol (Op 1) with our real external IP/port
                {
                    json selectP = { {"op", 1}, {"d", {{"protocol", "udp"}, {"data",
                        {{"address", myIp}, {"port", myPort}, {"mode", "aead_xchacha20_poly1305_rtpsize"}}}}} };
                    std::string sSel = selectP.dump();
                    DebugLog("[VOICE] Sending Select Protocol (Op 1): ip=" + myIp + " port=" + std::to_string(myPort));
                    { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                      WinHttpWebSocketSend((HINTERNET)hVoiceWS,
                          WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                          (void*)sSel.c_str(), (DWORD)sSel.size()); }
                }
            }

            else if (op == 4) {
                DebugLog("[VOICE] Received SESSION_DESCRIPTION (Op 4) - READY!");
                OutputDebugStringA("[VOICE] Received SESSION_DESCRIPTION (Op 4) - READY!\n");
                m_VoiceConn.m_SecretKey = d["secret_key"].get<std::vector<uint8_t>>();

                if (m_VoiceConn.m_Running && !m_VoiceConn.m_UdpReceiveThread.joinable()) {
                    DebugLog("[VOICE] Starting UdpReceiveLoop thread now that Session Description (Op 4) is received.");
                    m_VoiceConn.m_UdpReceiveThread = std::thread(&DiscordClient::UdpReceiveLoop, this);
                    
                    // Start correct UDP keepalive thread
                    uint32_t currentSsrc = m_VoiceConn.m_Ssrc;
                    std::thread([this, currentSsrc]() {
                        DebugLog("[UDP] Keepalive thread started (74-byte format)");
                        unsigned char disc_pkt[74] = { 0 };
                        *(uint16_t*)(disc_pkt)     = htons(1);
                        *(uint16_t*)(disc_pkt + 2) = htons(70);
                        *(uint32_t*)(disc_pkt + 4) = htonl(currentSsrc);
                        while (m_VoiceConn.m_Running) {
                            sendto(m_VoiceConn.m_UdpSocket, (char*)disc_pkt, 74, 0,
                                (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
                            for (int i = 0; i < 20 && m_VoiceConn.m_Running; ++i) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                        DebugLog("[UDP] Keepalive thread ended");
                    }).detach();
                    
                }

                if (d.contains("dave_protocol_version")) {
                    m_VoiceConn.m_DaveVersion = d["dave_protocol_version"].get<uint16_t>();
                    DebugLog("[VOICE] DAVE Protocol Negotiated: v" + std::to_string(m_VoiceConn.m_DaveVersion));
                    if (m_VoiceConn.m_DaveSession) {
                        uint64_t gId = 0;
                        try { gId = std::stoull(m_VoiceConn.m_ChannelId.empty() ? guildId : m_VoiceConn.m_ChannelId); }
                        catch (...) {}
                        
                        unsigned long long parsedUserId = 0;
                        try { parsedUserId = std::stoull(userId); }
                        catch (const std::exception& e) {
                            DebugLog("[VOICE] userId std::stoull threw exception: " + std::string(e.what()));
                        }
                        char hexBuf[32];
                        sprintf_s(hexBuf, "0x%llX", parsedUserId);
                        DebugLog("[VOICE] daveSessionInit: userId='" + userId + "' len=" + std::to_string(userId.length()) + 
                                 " parsed_uint64=" + std::to_string(parsedUserId) + " hex=" + std::string(hexBuf));

                        daveSessionInit((DAVESessionHandle)m_VoiceConn.m_DaveSession, m_VoiceConn.m_DaveVersion, gId, userId.c_str());
                        DebugLog("[VOICE] daveSessionInit called for group: " + std::to_string(gId));
                        uint8_t* kp = nullptr;
                        size_t kpLen = 0;
                        daveSessionGetMarshalledKeyPackage((DAVESessionHandle)m_VoiceConn.m_DaveSession, &kp, &kpLen);
                        DebugLog("[VOICE] KeyPackage: ptr=" + std::to_string((uintptr_t)kp) + " len=" + std::to_string(kpLen));
                        if (kp && kpLen > 0) {
                            std::vector<uint8_t> msg = { 26 };
                            msg.insert(msg.end(), kp, kp + kpLen);
                            { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                              WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, msg.data(), (DWORD)msg.size()); }
                            daveFree(kp);
                            DebugLog("[VOICE] Sent MLS_KEY_PACKAGE (Op 26), bytes=" + std::to_string(msg.size()));
                        }
                        else {
                            DebugLog("[VOICE] ERROR: KeyPackage is NULL or zero-length!");
                        }
                    }
                    if (!m_VoiceConn.m_DaveEncryptor) {
                        m_VoiceConn.m_DaveEncryptor = daveEncryptorCreate();
                        daveEncryptorAssignSsrcToCodec((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor, m_VoiceConn.m_Ssrc, DAVE_CODEC_OPUS);
                    }
                    m_VoiceConn.m_Ready = true;
                    
                    // Send Op 5 (Speaking) to let the server know we are ready
                    json speaking = { {"op", 5}, {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", m_VoiceConn.m_Ssrc}}} };
                    std::string sSp = speaking.dump();
                    { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                      WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sSp.c_str(), (DWORD)sSp.size()); }
                    DebugLog("[VOICE] Sent Op 5 (Speaking)");
                }
                else {
                    m_VoiceConn.m_Ready = true;
                    m_VoiceReady = true;
                    std::thread(&DiscordClient::AudioCaptureLoop, this).detach();
                }
            }
            else if (op == 8) {
                DebugLog("[VOICE] Received HELLO (Op 8). Syncing (v8 Post-E2EE)...");
                int interval = j["d"]["heartbeat_interval"];
                DebugLog("[VOICE] Handshake - Using Session ID: " + sessionId);
                json identify;
                identify["op"] = 0;
                json id;
                id["server_id"] = guildId.empty() ? m_VoiceConn.m_ChannelId : guildId;
                id["user_id"] = userId;
                id["session_id"] = sessionId;
                id["token"] = token;
                id["video"] = false;
                id["streams"] = json::array();
                // Real Discord desktop client sends capabilities=7 for voice identify
                // (bits: 1=PRIORITY_SPEAKER, 2=SYNC_SSRCS, 4=CONTEXT_AUDIO)
                id["capabilities"] = 7;
                id["max_dave_protocol_version"] = m_VoiceConn.m_DaveVersion;
                identify["d"] = id;
                std::string sId = identify.dump();
                DebugLog("[VOICE] Sending Identify: " + sId);
                { std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                  WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sId.c_str(), (DWORD)sId.size()); }
                std::thread([this, hVoiceWS, interval]() {
                    // First heartbeat at interval/2 (~6875ms): avoids the ~10s server-side
                    // timeout while not sending before the handshake is underway.
                    // Subsequent beats follow the normal interval.
                    int firstBeat = interval / 2;
                    std::this_thread::sleep_for(std::chrono::milliseconds(firstBeat));
                    if (!m_VoiceConn.m_Running || m_VoiceConn.m_hVoiceWS != hVoiceWS) return;
                    
                    while (m_VoiceConn.m_Running && m_VoiceConn.m_hVoiceWS == hVoiceWS) {
                        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        json hb = { {"op", 3}, {"d", { {"t", now_ms}, {"seq_ack", m_VoiceConn.m_VoiceSeqAck} }} };
                        std::string sHb = hb.dump();
                        {
                            std::lock_guard<std::mutex> lkSend(m_VoiceConn.m_VoiceWsSendMutex);
                            WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sHb.c_str(), (DWORD)sHb.size());
                        }
                        DebugLog("[VOICE] Sent voice heartbeat.");
                        for (int i = 0; i < interval / 100 && m_VoiceConn.m_Running && m_VoiceConn.m_hVoiceWS == hVoiceWS; ++i)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    }).detach();
            }
            else if (op == 5) {
                if (d.contains("user_id") && d["user_id"].is_string() && d.contains("ssrc") && d["ssrc"].is_number()) {
                    std::string uid = d["user_id"].get<std::string>();
                    uint32_t ssrc = d["ssrc"].get<uint32_t>();
                    std::lock_guard<std::mutex> lock(m_VoiceConn.m_VoiceDataMutex);
                    m_VoiceConn.m_SsrcToUser[ssrc] = uid;
                    DebugLog("[VOICE] Mapped SSRC " + std::to_string(ssrc) + " to User " + uid);
                }
            }
            else if (op == 11) {
                if (j["d"].contains("user_ids")) {
                    for (const auto& uid : j["d"]["user_ids"]) {
                        std::string user_id = uid.get<std::string>();
                        if (std::find(m_VoiceConn.m_RecognizedUserIds.begin(), m_VoiceConn.m_RecognizedUserIds.end(), user_id) == m_VoiceConn.m_RecognizedUserIds.end()) {
                            m_VoiceConn.m_RecognizedUserIds.push_back(user_id);
                        }
                    }
                    DebugLog("[VOICE] Recognized users count: " + std::to_string(m_VoiceConn.m_RecognizedUserIds.size()));
                }
            }
            else if (op == 13) {
                if (j["d"].contains("user_id")) {
                    std::string uid = j["d"]["user_id"].get<std::string>();
                    auto& v = m_VoiceConn.m_RecognizedUserIds;
                    v.erase(std::remove(v.begin(), v.end(), uid), v.end());
                    DebugLog("[VOICE] Client Disconnect: " + uid);
                    
                    std::lock_guard<std::mutex> lock(m_VoiceConn.m_VoiceDataMutex);
                    auto it = m_VoiceConn.m_UserDecryptors.find(uid);
                    if (it != m_VoiceConn.m_UserDecryptors.end()) {
                        daveDecryptorDestroy((DAVEDecryptorHandle)it->second);
                        m_VoiceConn.m_UserDecryptors.erase(it);
                        DebugLog("[DAVE] Destroyed decryptor for disconnected user: " + uid);
                    }
                }
            }
            
        }
        catch (...) {}
    }
    m_VoiceReady = false;
    m_VoiceConn.m_Ready = false;
    delete[] pbBuffer;
    closesocket(udpSocket);
    {
        std::lock_guard<std::mutex> lock(m_WsMutex);
        if (m_VoiceConn.m_hVoiceWS) {
            WinHttpWebSocketClose((HINTERNET)m_VoiceConn.m_hVoiceWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle((HINTERNET)m_VoiceConn.m_hVoiceWS);
            m_VoiceConn.m_hVoiceWS = nullptr;
        }
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (m_VoiceConn.m_DaveSession) {
        daveSessionDestroy((DAVESessionHandle)m_VoiceConn.m_DaveSession);
        m_VoiceConn.m_DaveSession = nullptr;
    }
    if (m_VoiceConn.m_DaveEncryptor) {
        daveEncryptorDestroy((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor);
        m_VoiceConn.m_DaveEncryptor = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_VoiceConn.m_VoiceDataMutex);
        for (auto& pair : m_VoiceConn.m_UserDecryptors) {
            if (pair.second) {
                daveDecryptorDestroy((DAVEDecryptorHandle)pair.second);
            }
        }
        m_VoiceConn.m_UserDecryptors.clear();
        for (auto& pair : m_VoiceConn.m_OpusDecoders) {
            if (pair.second) {
                opus_decoder_destroy((OpusDecoder*)pair.second);
            }
        }
        m_VoiceConn.m_OpusDecoders.clear();
        m_VoiceConn.m_SsrcToUser.clear();
        m_VoiceConn.m_UserPcmQueues.clear();
    }
    m_VoiceConn.m_Running = false;
    if (m_VoiceConn.m_UdpReceiveThread.joinable()) {
        m_VoiceConn.m_UdpReceiveThread.join();
    }
    if (m_VoiceConn.m_PlaybackThread.joinable()) {
        m_VoiceConn.m_PlaybackThread.join();
    }
    DebugLog("[VOICE] Loop finished and handles cleaned.");
}

void DiscordClient::AudioCaptureLoop() {
    int samples = 48000;
    int channels = 1;
    OpusEncoder* encoder = opus_encoder_create(samples, channels, OPUS_APPLICATION_VOIP, nullptr);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    uint16_t seq = 0;
    uint32_t ts = 0;
    unsigned char rtp_packet[2048];
    
    // waveIn setup
    HANDLE hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HWAVEIN hWaveIn = nullptr;
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = 96000;
    
    UINT inputDeviceIdx = (UINT)m_VoiceConn.m_InputDevice;
    if (waveInOpen(&hWaveIn, inputDeviceIdx, &wfx, (DWORD_PTR)hCaptureEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        // Fallback to WAVE_MAPPER
        waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)hCaptureEvent, 0, CALLBACK_EVENT);
    }
    
    const int NUM_CAPTURE_BUFFERS = 4;
    const int CAPTURE_BUFFER_SIZE = 1920; // 20ms of 48kHz 16-bit Mono = 960 samples * 2 bytes
    std::vector<WAVEHDR> headers(NUM_CAPTURE_BUFFERS);
    std::vector<std::vector<char>> buffers(NUM_CAPTURE_BUFFERS, std::vector<char>(CAPTURE_BUFFER_SIZE));
    
    bool waveInStarted = false;
    if (hWaveIn) {
        for (int i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
            headers[i].lpData = buffers[i].data();
            headers[i].dwBufferLength = CAPTURE_BUFFER_SIZE;
            waveInPrepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
            waveInAddBuffer(hWaveIn, &headers[i], sizeof(WAVEHDR));
        }
        waveInStart(hWaveIn);
        waveInStarted = true;
        DebugLog("[VOICE] waveIn started successfully.");
    } else {
        DebugLog("[VOICE] waveIn failed to initialize, falling back to silent frames.");
    }
    
    while (m_VoiceConn.m_Running && m_VoiceReady) {
        short pcm_in[960] = {0};
        bool gotInput = false;
        
        if (waveInStarted && hWaveIn) {
            DWORD waitRes = WaitForSingleObject(hCaptureEvent, 100);
            if (waitRes == WAIT_OBJECT_0) {
                for (int i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
                    if (headers[i].dwFlags & WHDR_DONE) {
                        waveInUnprepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
                        
                        // Copy data
                        memcpy(pcm_in, headers[i].lpData, CAPTURE_BUFFER_SIZE);
                        gotInput = true;
                        
                        // Re-queue
                        waveInPrepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
                        waveInAddBuffer(hWaveIn, &headers[i], sizeof(WAVEHDR));
                    }
                }
            }
        }
        
        if (!gotInput) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        
        if (m_VoiceConn.m_IsMuted) continue;
        
        // Encode Opus
        unsigned char opus_data[1024];
        int opus_len = opus_encode(encoder, pcm_in, 960, opus_data, sizeof(opus_data));
        if (opus_len <= 0) continue;
        
        // Construct RTP packet
        rtp_packet[0] = 0x80;
        rtp_packet[1] = 0x78;
        *(uint16_t*)(rtp_packet + 2) = htons(seq++);
        *(uint32_t*)(rtp_packet + 4) = htonl(ts);
        *(uint32_t*)(rtp_packet + 8) = htonl(m_VoiceConn.m_Ssrc);
        ts += 960;
        
        // 1. DAVE E2EE Layer
        unsigned char dave_encrypted_buf[1024] = {0};
        size_t dave_encrypted_len = 0;
        bool dave_ok = false;
        if (m_VoiceConn.m_DaveEncryptor && daveEncryptorHasKeyRatchet((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor)) {
            if (daveEncryptorEncrypt((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor, DAVE_MEDIA_TYPE_AUDIO, m_VoiceConn.m_Ssrc,
                opus_data, opus_len, dave_encrypted_buf, sizeof(dave_encrypted_buf), &dave_encrypted_len) == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
                dave_ok = true;
            }
        }
        
        const uint8_t* payload_ptr = dave_ok ? dave_encrypted_buf : opus_data;
        size_t payload_len = dave_ok ? dave_encrypted_len : (size_t)opus_len;
        
        // 2. Transport AEAD Layer (aead_xchacha20_poly1305_rtpsize)
        if (m_VoiceConn.m_SecretKey.size() == 32) {
            unsigned char nonce[24] = {0};
            uint32_t current_counter = m_VoiceConn.m_TxCounter++;
            uint32_t network_counter = htonl(current_counter);
            *(uint32_t*)nonce = network_counter;
            
            unsigned long long ciphertext_len = 0;
            crypto_aead_xchacha20poly1305_ietf_encrypt(
                rtp_packet + 12,
                &ciphertext_len,
                payload_ptr,
                payload_len,
                rtp_packet, // AAD = RTP header (12 bytes)
                12,
                nullptr,
                nonce,
                m_VoiceConn.m_SecretKey.data()
            );
            
            // Append 4-byte counter at the end
            memcpy(rtp_packet + 12 + ciphertext_len, &network_counter, 4);
            
            sendto(m_VoiceConn.m_UdpSocket, (char*)rtp_packet, 12 + (int)ciphertext_len + 4, 0,
                (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
        }
    }
    
    // waveIn cleanup
    if (hWaveIn) {
        waveInReset(hWaveIn);
        for (int i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
            waveInUnprepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
        }
        waveInClose(hWaveIn);
    }
    CloseHandle(hCaptureEvent);
    opus_encoder_destroy(encoder);
}

// ---------------------------------------------------------------------------
// UpdateDaveKeys - refresh encryptor + all per-user decryptors after MLS epoch
// ---------------------------------------------------------------------------
void DiscordClient::UpdateDaveKeys() {
    if (!m_VoiceConn.m_DaveSession) return;

    // 1. Update our own encryptor key ratchet
    if (m_VoiceConn.m_DaveEncryptor) {
        // We always use "self" key for our own SSRC
        std::string selfId;
        { std::lock_guard<std::mutex> lock(m_IdMutex); selfId = m_UserId; }

        DAVEKeyRatchetHandle ratchet = daveSessionGetKeyRatchet(
            (DAVESessionHandle)m_VoiceConn.m_DaveSession, selfId.c_str());
        if (ratchet) {
            daveEncryptorSetKeyRatchet((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor, ratchet);
            daveKeyRatchetDestroy(ratchet);
            DebugLog("[DAVE] Encryptor key ratchet updated for self: " + selfId);
        } else {
            DebugLog("[DAVE] WARNING: Could not get key ratchet for self");
        }
    }

    // 2. Update/create per-user decryptors
    std::lock_guard<std::mutex> lock(m_VoiceConn.m_VoiceDataMutex);
    for (const auto& uid : m_VoiceConn.m_RecognizedUserIds) {
        std::string selfId;
        { std::lock_guard<std::mutex> idLock(m_IdMutex); selfId = m_UserId; }
        if (uid == selfId) continue; // skip self

        DAVEKeyRatchetHandle ratchet = daveSessionGetKeyRatchet(
            (DAVESessionHandle)m_VoiceConn.m_DaveSession, uid.c_str());
        if (!ratchet) {
            DebugLog("[DAVE] No ratchet yet for user: " + uid);
            continue;
        }

        // Create decryptor if it doesn't exist
        if (m_VoiceConn.m_UserDecryptors.find(uid) == m_VoiceConn.m_UserDecryptors.end()) {
            m_VoiceConn.m_UserDecryptors[uid] = daveDecryptorCreate();
            DebugLog("[DAVE] Created new decryptor for user: " + uid);
        }

        daveDecryptorTransitionToKeyRatchet(
            (DAVEDecryptorHandle)m_VoiceConn.m_UserDecryptors[uid], ratchet);
        daveKeyRatchetDestroy(ratchet);
        DebugLog("[DAVE] Decryptor key ratchet updated for user: " + uid);
    }
}

// ---------------------------------------------------------------------------
// UdpReceiveLoop - receive RTP packets, decrypt transport + DAVE, Opus decode
// ---------------------------------------------------------------------------
void DiscordClient::UdpReceiveLoop() {
    DebugLog("[UDP] UdpReceiveLoop started");
    std::vector<uint8_t> pkt(4096);

    while (m_VoiceConn.m_Running) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(m_VoiceConn.m_UdpSocket,
            (char*)pkt.data(), (int)pkt.size(), 0,
            (sockaddr*)&from, &fromLen);

        if (n <= 0) {
            // SO_RCVTIMEO fires a WSAETIMEDOUT here; just loop
            continue;
        }

        // ---- Silently drop IP-discovery response (type=2) ----
        if (n >= 2 && pkt[0] == 0x00 && pkt[1] == 0x02) continue;

        // ---- Minimum valid RTP packet: 12-byte header ----
        if (n < 12) continue;

        // Parse RTP header
        uint8_t  rtpV   = (pkt[0] >> 6) & 0x3;
        if (rtpV != 2) continue;           // not RTP v2

        // Filter out RTCP packets (payload type 72-76 = RTCP over RTP port).
        // These pass the version check but are not encrypted RTP audio data.
        uint8_t rtpPT = pkt[1] & 0x7F;
        if (rtpPT >= 72 && rtpPT <= 76) continue;

        bool     rtpX   = (pkt[0] >> 4) & 0x1;
        uint8_t  rtpCC  = pkt[0] & 0x0F;
        uint32_t ssrc   = (pkt[8]<<24)|(pkt[9]<<16)|(pkt[10]<<8)|pkt[11];

        // Skip our own SSRC
        if (ssrc == m_VoiceConn.m_Ssrc) continue;

        // Skip CSRC list. In _rtpsize modes, the base header (12 + CC*4) serves as AAD.
        // If the X (extension) bit is set, the 4-byte extension preamble is also AAD,
        // but the extension payload itself is encrypted (part of the ciphertext).
        int headerLen = 12 + rtpCC * 4;
        int aadLen = headerLen;
        int extPayloadLen = 0;
        if (rtpX && n >= headerLen + 4) {
            uint16_t extLen = (pkt[headerLen + 2] << 8) | pkt[headerLen + 3];
            aadLen += 4;
            extPayloadLen = extLen * 4;
        }
        if (aadLen + extPayloadLen >= n) continue;

        // The ciphertext starts after the AAD.
        // Minimum packet size includes AAD, encrypted extension payload, ciphertext overhead, and 4-byte counter.
        if (n < aadLen + extPayloadLen + 4 + (int)crypto_aead_xchacha20poly1305_ietf_ABYTES) continue;

        if (m_VoiceConn.m_SecretKey.size() != 32) continue;

        // Last 4 bytes are the 32-bit nonce counter
        const uint8_t* rawEnd  = pkt.data() + n;
        const uint8_t* nonceTag = rawEnd - 4;
        uint8_t nonce[24] = {0};
        memcpy(nonce, nonceTag, 4);

        const uint8_t* cipherStart = pkt.data() + aadLen;
        size_t cipherLen = (size_t)(nonceTag - cipherStart);

        std::vector<uint8_t> plaintext(cipherLen);
        unsigned long long ptLen = 0;

        int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &ptLen,
            nullptr,
            cipherStart, cipherLen,
            pkt.data(), aadLen,   // AAD = RTP header + optional extension preamble
            nonce,
            m_VoiceConn.m_SecretKey.data());

        if (rc != 0) {
            DebugLog("[UDP] Transport decrypt failed for SSRC=" + std::to_string(ssrc));
            continue;
        }
        plaintext.resize((size_t)ptLen);

        // Strip the decrypted extension payload to get the actual DAVE/Opus payload
        if (extPayloadLen > (int)plaintext.size()) continue;
        const uint8_t* davePayloadPtr = plaintext.data() + extPayloadLen;
        size_t davePayloadLen = plaintext.size() - extPayloadLen;

        // ---- DAVE E2EE decrypt layer ----
        std::vector<uint8_t> opusData;

        std::string uid;
        {
            std::lock_guard<std::mutex> lk(m_VoiceConn.m_VoiceDataMutex);
            auto it = m_VoiceConn.m_SsrcToUser.find(ssrc);
            if (it != m_VoiceConn.m_SsrcToUser.end()) uid = it->second;
        }

        bool daveDecrypted = false;
        if (!uid.empty() && m_VoiceConn.m_DaveHandshakeComplete) {
            std::lock_guard<std::mutex> lk(m_VoiceConn.m_VoiceDataMutex);
            auto dit = m_VoiceConn.m_UserDecryptors.find(uid);
            if (dit != m_VoiceConn.m_UserDecryptors.end() && dit->second) {
                size_t maxPt = daveDecryptorGetMaxPlaintextByteSize(
                    (DAVEDecryptorHandle)dit->second,
                    DAVE_MEDIA_TYPE_AUDIO, davePayloadLen);
                opusData.resize(maxPt);
                size_t written = 0;
                auto drc = daveDecryptorDecrypt(
                    (DAVEDecryptorHandle)dit->second,
                    DAVE_MEDIA_TYPE_AUDIO,
                    davePayloadPtr, davePayloadLen,
                    opusData.data(), maxPt, &written);
                if (drc == DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) {
                    opusData.resize(written);
                    daveDecrypted = true;
                } else {
                    DebugLog("[DAVE] Decrypt failed for uid=" + uid + " rc=" + std::to_string((int)drc));
                }
            }
        }

        if (!daveDecrypted) {
            // Either DAVE not active yet (pre-handshake) or no decryptor → treat as raw Opus
            opusData.assign(davePayloadPtr, davePayloadPtr + davePayloadLen);
        }

        if (opusData.empty()) continue;

        // ---- Opus decode ----
        {
            std::lock_guard<std::mutex> lk(m_VoiceConn.m_VoiceDataMutex);

            // Create decoder if needed
            if (m_VoiceConn.m_OpusDecoders.find(ssrc) == m_VoiceConn.m_OpusDecoders.end()) {
                int err = 0;
                OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
                if (err == OPUS_OK && dec) {
                    m_VoiceConn.m_OpusDecoders[ssrc] = dec;
                    DebugLog("[UDP] Created Opus decoder for SSRC=" + std::to_string(ssrc));
                }
            }

            auto decIt = m_VoiceConn.m_OpusDecoders.find(ssrc);
            if (decIt == m_VoiceConn.m_OpusDecoders.end() || !decIt->second) continue;

            // 960 samples * 2 channels = 1920 shorts (stereo)
            std::vector<int16_t> pcm(960 * 2);
            int frames = opus_decode(
                (OpusDecoder*)decIt->second,
                opusData.data(), (opus_int32)opusData.size(),
                pcm.data(), 960, 0);

            if (frames > 0) {
                auto& queue = m_VoiceConn.m_UserPcmQueues[ssrc];
                // Cap queue to ~1 second of audio to prevent unbounded growth
                const size_t MAX_QUEUE_SAMPLES = 48000 * 2 * 1; // 1s stereo
                size_t newSamples = (size_t)(frames * 2);
                if (queue.size() + newSamples <= MAX_QUEUE_SAMPLES) {
                    for (int s = 0; s < frames * 2; ++s)
                        queue.push_back(pcm[s]);
                }
            }
        }
    }
    DebugLog("[UDP] UdpReceiveLoop ended");
}

// ---------------------------------------------------------------------------
// AudioPlaybackLoop - mix per-user PCM queues and play via waveOut
// ---------------------------------------------------------------------------
void DiscordClient::AudioPlaybackLoop() {
    DebugLog("[PLAYBACK] AudioPlaybackLoop started");

    const int SAMPLE_RATE = 48000;
    const int CHANNELS    = 2;
    const int FRAME_SIZE  = 960;   // 20ms at 48kHz
    const int BUFFER_BYTES = FRAME_SIZE * CHANNELS * sizeof(int16_t); // 3840 bytes
    const int NUM_BUFFERS  = 8;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = (WORD)CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = (WORD)(CHANNELS * sizeof(int16_t));
    wfx.nAvgBytesPerSec = SAMPLE_RATE * wfx.nBlockAlign;

    HANDLE hPlayEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT hWaveOut = nullptr;

    UINT outputDeviceIdx = (UINT)m_VoiceConn.m_OutputDevice;
    if (waveOutOpen(&hWaveOut, outputDeviceIdx, &wfx,
                    (DWORD_PTR)hPlayEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx,
                    (DWORD_PTR)hPlayEvent, 0, CALLBACK_EVENT);
    }

    if (!hWaveOut) {
        DebugLog("[PLAYBACK] waveOutOpen failed – playback disabled");
        CloseHandle(hPlayEvent);
        return;
    }
    DebugLog("[PLAYBACK] waveOut opened successfully");

    // Allocate double-buffered waveOut buffers
    std::vector<WAVEHDR>              waveHdrs(NUM_BUFFERS);
    std::vector<std::vector<int16_t>> audioBufs(NUM_BUFFERS,
        std::vector<int16_t>(FRAME_SIZE * CHANNELS, 0));

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        waveHdrs[i] = {};
        waveHdrs[i].lpData         = (LPSTR)audioBufs[i].data();
        waveHdrs[i].dwBufferLength = BUFFER_BYTES;
        waveOutPrepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
        // Pre-fill with silence and queue immediately so waveOut starts smoothly
        waveOutWrite(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
    }

    while (m_VoiceConn.m_Running) {
        // Wait for a buffer to become free (or 50ms timeout)
        WaitForSingleObject(hPlayEvent, 50);

        for (int i = 0; i < NUM_BUFFERS; ++i) {
            if (!(waveHdrs[i].dwFlags & WHDR_DONE)) continue;

            // Mix all user PCM queues into this buffer
            std::fill(audioBufs[i].begin(), audioBufs[i].end(), (int16_t)0);

            if (!m_VoiceConn.m_IsDeafened) {
                std::lock_guard<std::mutex> lk(m_VoiceConn.m_VoiceDataMutex);
                const int SAMPLES_NEEDED = FRAME_SIZE * CHANNELS;
                float vol = m_VoiceConn.m_OutputVolume;

                for (auto& [ssrc, queue] : m_VoiceConn.m_UserPcmQueues) {
                    if (queue.empty()) continue;

                    int toDrain = (std::min)((int)queue.size(), SAMPLES_NEEDED);
                    for (int s = 0; s < toDrain; ++s) {
                        int32_t sample = (int32_t)((float)queue.front() * vol);
                        queue.pop_front();
                        int32_t mixed = (int32_t)audioBufs[i][s] + sample;
                        // Clamp to int16 range
                        if (mixed >  32767) mixed =  32767;
                        if (mixed < -32768) mixed = -32768;
                        audioBufs[i][s] = (int16_t)mixed;
                    }
                }
            }

            // Re-submit the buffer
            waveHdrs[i].dwFlags      &= ~WHDR_DONE;
            waveHdrs[i].dwBufferLength = BUFFER_BYTES;
            waveOutWrite(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
        }
    }

    // Cleanup
    waveOutReset(hWaveOut);
    for (int i = 0; i < NUM_BUFFERS; ++i)
        waveOutUnprepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
    waveOutClose(hWaveOut);
    CloseHandle(hPlayEvent);
    DebugLog("[PLAYBACK] AudioPlaybackLoop ended");
}

void DiscordClient::HeartbeatLoop() {
    while (m_RunHeartbeat) {
        json heartbeat = { {"op", 1}, {"d", m_SequenceNumber > 0 ? json(m_SequenceNumber) : nullptr} };
        QueueWsMessage(heartbeat.dump());
        for (int i = 0; i < m_HeartbeatInterval && m_RunHeartbeat; i += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DiscordClient::QueueWsMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_SendMutex);
    m_WsSendQueue.push(message);
    m_SendCv.notify_one();
}

void DiscordClient::SendThreadLoop() {
    while (m_SendThreadRunning) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lock(m_SendMutex);
            m_SendCv.wait(lock, [this] { return !m_WsSendQueue.empty() || !m_SendThreadRunning; });
            if (!m_SendThreadRunning) break;
            msg = m_WsSendQueue.front();
            m_WsSendQueue.pop();
        }
        HINTERNET hWS = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_WsMutex);
            if (m_Connected) hWS = (HINTERNET)m_hWebSocket;
        }
        if (hWS)
            WinHttpWebSocketSend(hWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)msg.c_str(), (DWORD)msg.size());
    }
}

void DiscordClient::ParseVoiceStateUpdate(const nlohmann::json& d) {
    std::string userId = d.value("user_id", "");
    std::string channelId = (d.contains("channel_id") && !d["channel_id"].is_null()) ? d["channel_id"].get<std::string>() : "";
    DebugLog("[GATEWAY] ParseVoiceStateUpdate: user=" + userId + " channel=" + channelId);
    std::string displayName = "User " + userId.substr(0, 4);
    std::string avatarHash = "";
    std::string username = "";
    if (d.contains("member") && !d["member"].is_null()) {
        const auto& mem = d["member"];
        if (mem.contains("nick") && !mem["nick"].is_null()) displayName = mem["nick"].get<std::string>();
        if (mem.contains("user") && !mem["user"].is_null()) {
            const auto& u = mem["user"];
            if (mem["nick"].is_null() || !mem.contains("nick")) {
                if (u.contains("global_name") && !u["global_name"].is_null()) displayName = u["global_name"].get<std::string>();
                else if (u.contains("username")) displayName = u["username"].get<std::string>();
            }
            if (u.contains("avatar") && !u["avatar"].is_null()) avatarHash = u["avatar"].get<std::string>();
            if (u.contains("username")) username = u["username"].get<std::string>();
        }
    }
    else if (d.contains("user") && !d["user"].is_null()) {
        const auto& u = d["user"];
        if (u.contains("global_name") && !u["global_name"].is_null()) displayName = u["global_name"].get<std::string>();
        else if (u.contains("username")) displayName = u["username"].get<std::string>();
        if (u.contains("avatar") && !u["avatar"].is_null()) avatarHash = u["avatar"].get<std::string>();
        if (u.contains("username")) username = u["username"].get<std::string>();
    }
    std::lock_guard<std::mutex> vl(m_VoiceMutex);
    if (channelId.empty()) {
        DebugLog("[GATEWAY] User " + userId + " left channel. Clearing.");
        for (auto it = m_VoiceMembers.begin(); it != m_VoiceMembers.end(); ++it) {
            if (it->m_Id == userId) { m_VoiceMembers.erase(it); break; }
        }
    }
    else {
        bool found = false;
        for (auto& m : m_VoiceMembers) {
            if (m.m_Id == userId) {
                m.m_IsMuted = d.value("mute", false) || d.value("self_mute", false);
                m.m_IsDeafened = d.value("deaf", false) || d.value("self_deaf", false);
                m.m_ChannelId = channelId;
                if (!displayName.empty()) m.m_DisplayName = displayName;
                if (!avatarHash.empty()) m.m_AvatarHash = avatarHash;
                found = true;
                break;
            }
        }
        if (!found) {
            VoiceMember vm;
            vm.m_Id = userId;
            vm.m_Username = username.empty() ? ("User " + userId.substr(0, 4)) : username;
            vm.m_DisplayName = displayName;
            vm.m_AvatarHash = avatarHash;
            vm.m_ChannelId = channelId;
            vm.m_IsMuted = d.value("mute", false) || d.value("self_mute", false);
            vm.m_IsDeafened = d.value("deaf", false) || d.value("self_deaf", false);
            m_VoiceMembers.push_back(vm);
            DebugLog("[GATEWAY] Added new voice member. Total: " + std::to_string(m_VoiceMembers.size()));
        }
    }
}

bool DiscordClient::StartCall(const std::string& channel_id) {
    DebugLog("[CALL] Starting DM call in channel: " + channel_id);
    JoinVoiceChannel("", channel_id);
    std::string resp = HttpRequest("POST", "/api/v9/channels/" + channel_id + "/call/ring", "{\"recipients\":null}");
    DebugLog("[CALL] Ring request finished. Success: " + std::string(resp.empty() ? "Yes (204)" : "Yes"));
    return true;
}

bool DiscordClient::EndCall(const std::string& channel_id) {
    DebugLog("[CALL] Ending call in channel: " + channel_id);
    LeaveVoiceChannel("");
    return true;
}

bool DiscordClient::AcceptCall(const std::string& channel_id) {
    DebugLog("[CALL] Accepting call in channel: " + channel_id);
    return JoinVoiceChannel("", channel_id);
}

bool DiscordClient::DeclineCall(const std::string& channel_id) {
    DebugLog("[CALL] Declining call in channel: " + channel_id);
    std::string resp = HttpRequest("POST", "/api/v9/channels/" + channel_id + "/call/stop-ringing", "{\"recipients\":null}");
    return !resp.empty();
}