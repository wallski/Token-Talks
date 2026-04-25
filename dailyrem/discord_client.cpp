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
#include "gui.h"
#include "vendor/include/opus.h"
#include "vendor/include/sodium.h"
#include "vendor/libdave/include/dave/dave.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "opus.lib")
#pragma comment(lib, "libsodium.lib")
#pragma comment(lib, "vendor/libdave/lib/libdave.lib")

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
        {"client_version", "1.0.9150"}, {"os_version", "10.0.19045"}, {"os_arch", "x64"},
        {"system_locale", "en-US"}, {"client_build_number", 300125},
        {"native_build_number", 53231}, {"client_event_source", nullptr}}},
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
                
                // Op 25: External Sender
                if (op == 25 && payloadLen < 100) {
                    DebugLog("[VOICE] DAVE External Sender");
                    if (m_VoiceConn.m_DaveSession) {
                        daveSessionSetExternalSender(
                            (DAVESessionHandle)m_VoiceConn.m_DaveSession, 
                            payload, payloadLen
                        );
                        
                        // Subscribe to join the group
                        json sub = {{"op", 16}, {"d", {
                            {"audio_ssrc", m_VoiceConn.m_Ssrc},
                            {"video_ssrc", 0}, {"rtx_ssrc", 0}
                        }}};
                        std::string s = sub.dump();
                        WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                            (void*)s.c_str(), (DWORD)s.size());
                        DebugLog("[VOICE] Sent Subscribe (Op 16)");
                    }
                }
                
                // Op 30: MLS Welcome (joining existing call)
                else if (op == 30) {
                    DebugLog("[VOICE] MLS Welcome (Op 30), len=" + std::to_string(payloadLen));
                    
                    if (m_VoiceConn.m_DaveSession && payloadLen > 0) {
                        std::vector<const char*> users;
                        for (auto& u : m_VoiceConn.m_RecognizedUserIds) 
                            users.push_back(u.c_str());
                        
                        void* result = daveSessionProcessWelcome(
                            (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                            payload, payloadLen, users.data(), users.size()
                        );
                        
                        if (result) {
                            DebugLog("[VOICE] Welcome processed! Sending Op 23");
                            
                            // Send Transition Ready
                            uint8_t ready[4] = {0, 0, 23, 1};
                            WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                ready, 4);
                            
                            // Set encryption keys
                            if (m_VoiceConn.m_DaveEncryptor) {
                                void* kr = daveSessionGetKeyRatchet(
                                    (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                                    userId.c_str()
                                );
                                if (kr) {
                                    daveEncryptorSetKeyRatchet(
                                        (DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor,
                                        (DAVEKeyRatchetHandle)kr
                                    );
                                    daveKeyRatchetDestroy((DAVEKeyRatchetHandle)kr);
                                    DebugLog("[VOICE] Encryption keys set!");
                                }
                            }
                            daveWelcomeResultDestroy((DAVEWelcomeResultHandle)result);
                        }
                    }
                }
                
                // Op 27: Proposals (when YOU are coordinator)
                else if (op == 27) {
                    DebugLog("[VOICE] Proposals (Op 27), len=" + std::to_string(payloadLen));
                    
                    if (m_VoiceConn.m_DaveSession && payloadLen > 0) {
                        std::vector<const char*> users;
                        for (auto& u : m_VoiceConn.m_RecognizedUserIds)
                            users.push_back(u.c_str());
                        
                        uint8_t* cw = nullptr;
                        size_t cwLen = 0;
                        daveSessionProcessProposals(
                            (DAVESessionHandle)m_VoiceConn.m_DaveSession,
                            payload, payloadLen, users.data(), users.size(),
                            &cw, &cwLen
                        );
                        
                        if (cw && cwLen > 0) {
                            std::vector<uint8_t> msg = {0, 0, 28};
                            msg.insert(msg.end(), cw, cw + cwLen);
                            WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                msg.data(), (DWORD)msg.size());
                            DebugLog("[VOICE] Sent Commit/Welcome (Op 28)");
                            daveFree(cw);
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
                            uint8_t ready[4] = {0, 0, 23, (uint8_t)tid};
                            WinHttpWebSocketSend((HINTERNET)m_VoiceConn.m_hVoiceWS,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                ready, 4);
                            DebugLog("[VOICE] Sent Op 23 for transition " + std::to_string(tid));
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
                
                continue;
            }
            DebugLog("[VOICE RX RAW] " + wsMessage);
            auto j = json::parse(wsMessage);
            int op = j["op"];
            auto d = j["d"];
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
                unsigned char packet[74] = { 0 };
                *(uint16_t*)(packet) = htons(1);
                *(uint16_t*)(packet + 2) = htons(70);
                *(uint32_t*)(packet + 4) = htonl(ssrc);
                bool discovered = false;
                for (int retry = 0; retry < 5 && !discovered; ++retry) {
                    sendto(udpSocket, (char*)packet, 74, 0, (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
                    struct sockaddr_in from;
                    int fromLen = sizeof(from);
                    char resp2[74] = { 0 };
                    int r = recvfrom(udpSocket, resp2, 74, 0, (struct sockaddr*)&from, &fromLen);
                    if (r > 0) {
                        discovered = true;
                        char szIp[64] = { 0 };
                        for (int k = 0; k < 63; k++) {
                            char c = resp2[8 + k];
                            if (c == 0) break;
                            szIp[k] = c;
                        }
                        std::string myIp = szIp;
                        uint16_t myPort = ntohs(*(uint16_t*)(resp2 + 72));
                        json selectP = { {"op", 1}, {"d", {{"protocol", "udp"}, {"data",
                            {{"address", myIp}, {"port", (int)myPort}, {"mode", "aead_xchacha20_poly1305_rtpsize"}}}}} };
                        OutputDebugStringA("[VOICE] Discovery OK, sending Select Protocol\n");
                        std::string sSel = selectP.dump();
                        WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sSel.c_str(), (DWORD)sSel.size());
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
                if (!discovered) {
                    OutputDebugStringA("[VOICE] Discovery failed, sending fallback Select Protocol\n");
                    json selectP = { {"op", 1}, {"d", {{"protocol", "udp"}, {"data",
                        {{"address", ip}, {"port", port}, {"mode", "aead_xchacha20_poly1305_rtpsize"}}}}} };
                    std::string sSel = selectP.dump();
                    WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sSel.c_str(), (DWORD)sSel.size());
                }
            }
            else if (op == 4) {
                DebugLog("[VOICE] Received SESSION_DESCRIPTION (Op 4) - READY!");
                OutputDebugStringA("[VOICE] Received SESSION_DESCRIPTION (Op 4) - READY!\n");
                m_VoiceConn.m_SecretKey = d["secret_key"].get<std::vector<uint8_t>>();
                if (d.contains("dave_protocol_version")) {
                    m_VoiceConn.m_DaveVersion = d["dave_protocol_version"].get<uint16_t>();
                    DebugLog("[VOICE] DAVE Protocol Negotiated: v" + std::to_string(m_VoiceConn.m_DaveVersion));
                    if (m_VoiceConn.m_DaveSession) {
                        uint64_t gId = 0;
                        try { gId = std::stoull(m_VoiceConn.m_ChannelId.empty() ? guildId : m_VoiceConn.m_ChannelId); }
                        catch (...) {}
                        daveSessionInit((DAVESessionHandle)m_VoiceConn.m_DaveSession, m_VoiceConn.m_DaveVersion, gId, userId.c_str());
                        DebugLog("[VOICE] daveSessionInit called for group: " + std::to_string(gId));
                        uint8_t* kp = nullptr;
                        size_t kpLen = 0;
                        daveSessionGetMarshalledKeyPackage((DAVESessionHandle)m_VoiceConn.m_DaveSession, &kp, &kpLen);
                        DebugLog("[VOICE] KeyPackage: ptr=" + std::to_string((uintptr_t)kp) + " len=" + std::to_string(kpLen));
                        if (kp && kpLen > 0) {
                            std::vector<uint8_t> msg = { 0, 0, 26 };
                            msg.insert(msg.end(), kp, kp + kpLen);
                            WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, msg.data(), (DWORD)msg.size());
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
                id["capabilities"] = 32767;
                id["max_dave_protocol_version"] = m_VoiceConn.m_DaveVersion;
                identify["d"] = id;
                std::string sId = identify.dump();
                DebugLog("[VOICE] Sending Identify: " + sId);
                WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sId.c_str(), (DWORD)sId.size());
                std::thread([this, hVoiceWS, interval]() {
                    while (m_VoiceConn.m_Running && m_VoiceConn.m_hVoiceWS == hVoiceWS) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                        json hb = { {"op", 3}, {"d", GetTickCount()} };
                        std::string sHb = hb.dump();
                        WinHttpWebSocketSend(hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sHb.c_str(), (DWORD)sHb.size());
                    }
                    }).detach();
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
    m_VoiceConn.m_Running = false;
    DebugLog("[VOICE] Loop finished and handles cleaned.");
}

void DiscordClient::AudioCaptureLoop() {
    json speaking = { {"op", 5}, {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", m_VoiceConn.m_Ssrc}}} };
    std::string sSp = speaking.dump();
    if (m_VoiceConn.m_hVoiceWS)
        WinHttpWebSocketSend(m_VoiceConn.m_hVoiceWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)sSp.c_str(), (DWORD)sSp.size());
    int samples = 48000;
    int channels = 1;
    OpusEncoder* encoder = opus_encoder_create(samples, channels, OPUS_APPLICATION_VOIP, nullptr);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    uint16_t seq = 0;
    uint32_t ts = 0;
    unsigned char rtp_packet[1024];
    unsigned char silent_opus[3] = { 0xF8, 0xFF, 0xFE };
    while (m_VoiceConn.m_Running && m_VoiceReady) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (m_VoiceConn.m_IsMuted) continue;
        rtp_packet[0] = 0x80;
        rtp_packet[1] = 0x78;
        *(uint16_t*)(rtp_packet + 2) = htons(seq++);
        *(uint32_t*)(rtp_packet + 4) = htonl(ts);
        *(uint32_t*)(rtp_packet + 8) = htonl(m_VoiceConn.m_Ssrc);
        ts += 960;
        unsigned char encrypted[512] = { 0 };
        size_t encrypted_len = 0;
        bool dave_encrypted = false;
        if (m_VoiceConn.m_DaveEncryptor && daveEncryptorHasKeyRatchet((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor)) {
            if (daveEncryptorEncrypt((DAVEEncryptorHandle)m_VoiceConn.m_DaveEncryptor, DAVE_MEDIA_TYPE_AUDIO, m_VoiceConn.m_Ssrc,
                silent_opus, 3, encrypted, 512, &encrypted_len) == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
                dave_encrypted = true;
            }
        }
        if (dave_encrypted) {
            memcpy(rtp_packet + 12, encrypted, encrypted_len);
            sendto(m_VoiceConn.m_UdpSocket, (char*)rtp_packet, 12 + (int)encrypted_len, 0,
                (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
        }
        else if (m_VoiceConn.m_SecretKey.size() == 32) {
            unsigned char nonce[24] = { 0 };
            for (int i = 0; i < 24; ++i) nonce[i] = rand() % 256;
            crypto_secretbox_easy(encrypted, silent_opus, 3, nonce, m_VoiceConn.m_SecretKey.data());
            unsigned long long box_len = 3 + crypto_secretbox_MACBYTES;
            memcpy(rtp_packet + 12, encrypted, (size_t)box_len);
            memcpy(rtp_packet + 12 + box_len, nonce, 24);
            sendto(m_VoiceConn.m_UdpSocket, (char*)rtp_packet, 12 + (int)box_len + 24, 0,
                (struct sockaddr*)&m_VoiceConn.m_ServerAddr, sizeof(m_VoiceConn.m_ServerAddr));
        }
    }
    opus_encoder_destroy(encoder);
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