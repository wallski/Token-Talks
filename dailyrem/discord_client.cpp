#include "discord_client.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include "vendor/nlohmann/json.hpp"

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

DiscordClient::DiscordClient() : m_Connected(false), m_RunHeartbeat(false), m_HeartbeatInterval(41250), m_SequenceNumber(0), m_hWebSocket(nullptr) {}

DiscordClient::~DiscordClient() {
    Disconnect();
}

void DiscordClient::SetToken(const std::string& token) {
    m_Token = token;
}

std::string DiscordClient::GetToken() const {
    return m_Token;
}

std::string DiscordClient::GetUserId() const {
    return m_UserId;
}

bool DiscordClient::ValidateToken(const std::string& token) {
    DiscordClient temp;
    temp.SetToken(token);
    std::string resp = temp.HttpRequest("GET", "/api/v9/users/@me");
    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        if (j.contains("id") && j.contains("username")) return true;
    } catch(...) {}
    return false;
}

bool DiscordClient::Connect() {
    if (m_Token.empty()) return false;
    if (m_Connected) return true;

    std::string resp = HttpRequest("GET", "/api/v9/users/@me");
    if (!resp.empty()) {
        try {
            auto j = json::parse(resp);
            if (j.contains("id")) m_UserId = j["id"].get<std::string>();
        } catch(...) {}
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

    if (m_WsThread.joinable()) m_WsThread.join();
    if (m_HeartbeatThread.joinable()) m_HeartbeatThread.join();
}

void DiscordClient::SetOnMessageCallback(std::function<void(const DiscordMessage&)> cb) {
    m_MessageCallback = cb;
}

void DiscordClient::SetOnConnectedCallback(std::function<void()> cb) {
    m_ConnectedCallback = cb;
}

bool DiscordClient::IsConnected() const {
    return m_Connected;
}

std::string DiscordClient::HttpRequest(const std::string& method, const std::string& path, const std::string& body) {
    HINTERNET session = WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return "";

    HINTERNET connect = WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return ""; }

    std::wstring wPath(path.begin(), path.end());
    std::wstring wMethod(method.begin(), method.end());

    HINTERNET request = WinHttpOpenRequest(connect, wMethod.c_str(), wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ""; }

    std::string authHeader = "Authorization: " + m_Token + "\r\n";
    std::string contentHeader = "Content-Type: application/json\r\n";
    std::string combinedHeaders = authHeader + contentHeader;
    std::wstring wheaders(combinedHeaders.begin(), combinedHeaders.end());

    BOOL sent = WinHttpSendRequest(request, wheaders.c_str(), -1, (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ""; }

    WinHttpReceiveResponse(request, NULL);

    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &statusCode, &size, NULL);

    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
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
    if (resp.empty()) return guilds;

    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (const auto& item : j) {
                guilds.push_back({ item["id"].get<std::string>(), item["name"].get<std::string>() });
            }
        }
    } catch (...) {}
    return guilds;
}

std::vector<DiscordChannel> DiscordClient::FetchChannels(const std::string& guild_id) {
    std::vector<DiscordChannel> channels;
    std::string resp = HttpRequest("GET", "/api/v9/guilds/" + guild_id + "/channels");
    if (resp.empty()) return channels;

    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (const auto& item : j) {
                int type = item["type"].get<int>();
                if (type == 0 || type == 2) {
                    channels.push_back({ item["id"].get<std::string>(), item["name"].get<std::string>(), type });
                }
            }
        }
    } catch (...) {}
    return channels;
}

std::vector<DiscordMessage> DiscordClient::FetchMessages(const std::string& channel_id) {
    std::vector<DiscordMessage> msgs;
    std::string resp = HttpRequest("GET", "/api/v9/channels/" + channel_id + "/messages?limit=50");
    if (resp.empty()) return msgs;

    try {
        auto j = json::parse(resp);
        if (j.is_array()) {
            for (auto it = j.rbegin(); it != j.rend(); ++it) { // Reverse to get oldest to newest
                const auto& item = *it;
                std::string author = "Unknown";
                std::string author_id = "";
                if (item.contains("author")) {
                    if (item["author"].contains("username")) author = item["author"]["username"].get<std::string>();
                    if (item["author"].contains("id")) author_id = item["author"]["id"].get<std::string>();
                }
                
                std::string content = "";
                std::string attachment_url = "";
                std::string attachment_filename = "";
                if (item.contains("content"))
                    content = item["content"].get<std::string>();
                
                if (item.contains("attachments") && item["attachments"].is_array() && !item["attachments"].empty()) {
                    attachment_url = item["attachments"][0]["url"].get<std::string>();
                    attachment_filename = item["attachments"][0]["filename"].get<std::string>();
                } else if (item.contains("embeds") && item["embeds"].is_array() && !item["embeds"].empty()) {
                    if (item["embeds"][0].contains("thumbnail") && item["embeds"][0]["thumbnail"].contains("url")) {
                        attachment_url = item["embeds"][0]["thumbnail"]["url"].get<std::string>();
                        attachment_filename = "embed_image";
                    }
                }

                msgs.push_back({ item["id"].get<std::string>(), author, author_id, content, attachment_url, attachment_filename });
            }
        }
    } catch (...) {}
    return msgs;
}

bool DiscordClient::SendDiscordMessage(const std::string& channel_id, const std::string& content) {
    json payload = { {"content", content} };
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
    return !resp.empty(); // HTTP DELETE replies natively without body constraints usually depending on discord proxy
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

    HINTERNET hSession = WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<unsigned char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
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

    HINTERNET session = WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    
    std::string path = "/api/v9/channels/" + channel_id + "/messages";
    std::wstring wPath(path.begin(), path.end());
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    std::string headers = "Authorization: " + m_Token + "\r\n";
    headers += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
    std::wstring wheaders(headers.begin(), headers.end());

    BOOL sent = WinHttpSendRequest(request, wheaders.c_str(), -1, requestBuffer.data(), (DWORD)requestBuffer.size(), (DWORD)requestBuffer.size(), 0);
    if (sent) WinHttpReceiveResponse(request, NULL);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return sent == TRUE;
}

void DiscordClient::SendIdentify(void* hWebSocket) {
    json identify = {
        {"op", 2},
        {"d", {
            {"token", m_Token},
            {"capabilities", 16381},
            {"properties", {
                {"os", "Windows"},
                {"browser", "Chrome"},
                {"device", ""}
            }}
        }}
    };
    std::string payload = identify.dump();
    WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)payload.data(), (DWORD)payload.size());
}

void DiscordClient::WebSocketLoop() {
    HINTERNET hSession = WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, L"gateway.discord.gg", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/?v=9&encoding=json", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
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
    BYTE* pbBuffer = new BYTE[cbBuffer];

    while (m_Connected) {
        DWORD cbDataRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType;
        DWORD dwError = WinHttpWebSocketReceive(hWebSocket, pbBuffer, cbBuffer, &cbDataRead, &eBufferType);
        
        if (dwError != ERROR_SUCCESS) break;
        if (eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;

        if (cbDataRead > 0) {
            std::string msg((char*)pbBuffer, cbDataRead);
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
                }
                else if (op == 0) { // Dispatch
                    std::string t = j["t"].get<std::string>();
                    if (t == "READY") {
                        if (m_ConnectedCallback) m_ConnectedCallback();
                    }
                    else if (t == "MESSAGE_CREATE") {
                        if (m_MessageCallback) {
                            DiscordMessage dmsg;
                            dmsg.id = j["d"]["id"].get<std::string>();
                            if (j["d"].contains("author") && !j["d"]["author"].is_null()) {
                                if (j["d"]["author"].contains("username")) dmsg.author = j["d"]["author"]["username"].get<std::string>();
                                if (j["d"]["author"].contains("id")) dmsg.author_id = j["d"]["author"]["id"].get<std::string>();
                            }
                            dmsg.content = j["d"]["content"].get<std::string>();
                            m_MessageCallback(dmsg);
                        }
                    }
                }
            } catch (...) {}
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

void DiscordClient::HeartbeatLoop() {
    while (m_RunHeartbeat) {
        for(int i = 0; i < m_HeartbeatInterval && m_RunHeartbeat; i += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_RunHeartbeat) break;
        
    }
}
