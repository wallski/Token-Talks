#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct DiscordGuild {
    std::string id;
    std::string name;
};

struct DiscordChannel {
    std::string id;
    std::string name;
    int type;
};

struct DiscordMessage {
    std::string id;
    std::string author;
    std::string author_id;
    std::string content;
    std::string attachment_url;
    std::string attachment_filename;
};

class DiscordClient {
public:
    DiscordClient();
    ~DiscordClient();

    void SetToken(const std::string& token);
    std::string GetToken() const;
    std::string GetUserId() const;
    static bool ValidateToken(const std::string& token);

    bool Connect();
    void Disconnect();

    // HTTP Endpoints
    std::vector<DiscordGuild> FetchGuilds();
    std::vector<DiscordChannel> FetchChannels(const std::string& guild_id);
    std::vector<DiscordMessage> FetchMessages(const std::string& channel_id);
    bool SendDiscordMessage(const std::string& channel_id, const std::string& content);
    bool EditMessage(const std::string& channel_id, const std::string& msg_id, const std::string& new_content);
    bool DeleteMessage(const std::string& channel_id, const std::string& msg_id);
    bool SendAttachment(const std::string& channel_id, const std::string& filepath);
    std::vector<unsigned char> DownloadFile(const std::string& urlStr);

    // Callbacks
    void SetOnMessageCallback(std::function<void(const DiscordMessage&)> cb);
    void SetOnConnectedCallback(std::function<void()> cb);
    
    bool IsConnected() const;

private:
    std::string m_Token;
    std::atomic<bool> m_Connected;
    std::atomic<bool> m_RunHeartbeat;
    std::string m_UserId;
    
    std::thread m_WsThread;
    std::thread m_HeartbeatThread;
    
    std::function<void(const DiscordMessage&)> m_MessageCallback;
    std::function<void()> m_ConnectedCallback;

    int m_HeartbeatInterval;
    std::string m_SessionId;
    int m_SequenceNumber;

    void* m_hWebSocket = nullptr;
    std::mutex m_WsMutex;

    void WebSocketLoop();
    void HeartbeatLoop();
    void SendIdentify(void* hWebSocket);
    
    // Internal WinHTTP helpers
    std::string HttpRequest(const std::string& method, const std::string& path, const std::string& body = "");
};
