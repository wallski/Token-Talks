#pragma once

// winsock2 MUST come before windows.h to avoid redefinition errors
#define WIN32_LEAN_AND_MEAN
#define SODIUM_STATIC
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include "vendor/nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// Plain data structs (no class dependencies)
// ---------------------------------------------------------------------------

struct DiscordGuild {
    std::string id;
    std::string name;
    std::string icon_hash; // CDN: discord.com/icons/{id}/{hash}.png
};

struct DiscordChannel {
    std::string id;
    std::string name;
    int type;       // 0=text, 2=voice, 4=category
    bool is_locked;
    int  position = 0;
    std::string parent_id; // category id
};

struct DiscordReaction {
    std::string emoji;
    int count;
    bool me;
};

struct VoiceMember {
    std::string m_Id;
    std::string m_Username;    // fallback / unique handle
    std::string m_DisplayName; // global_name or server nick
    std::string m_AvatarHash;  // for CDN avatar URL
    std::string m_ChannelId;   // which VC they're in
    bool m_IsMuted    = false;
    bool m_IsDeafened = false;
    bool m_IsSpeaking = false; // green ring indicator
};

struct VoiceConnection {
    std::string m_Endpoint;
    std::string m_Token;
    std::string m_SessionId;
    std::string m_GuildId;
    std::string m_ChannelId;
    uint32_t    m_Ssrc = 0;
    std::vector<uint8_t> m_SecretKey;
    bool m_Ready   = false;
    bool m_Running = false;

    // UDP / Voice WebSocket
    SOCKET       m_UdpSocket = INVALID_SOCKET;
    HINTERNET    m_hVoiceWS  = nullptr;
    sockaddr_in  m_ServerAddr = {};
    std::thread  m_VoiceThread;

    int m_InputDevice = 0;
    int m_OutputDevice = 0;
    bool m_IsMuted = false;
    bool m_IsDeafened = false;
};

struct DiscordMessage {
    std::string id;
    std::string author;       // display name (global_name preferred)
    std::string author_id;
    std::string author_username; // raw username for fallback
    std::string author_avatar;   // avatar hash for CDN
    std::string timestamp;       // ISO8601 from Discord
    std::string content;
    std::vector<std::string>     attachment_urls;
    std::vector<std::string>     video_urls;
    std::vector<DiscordReaction> reactions;
};

// ---------------------------------------------------------------------------
// DiscordClient
// ---------------------------------------------------------------------------

class DiscordClient {
public:
    DiscordClient();
    ~DiscordClient();

    // Token / auth
    void        SetToken(const std::string& token);
    std::string GetToken()  const;
    std::string GetUserId() const;
    std::string GetUserName() const;
    std::string GetUserAvatar() const;
    static bool        ValidateToken(const std::string& token);
    static std::string LoginWithCredentials(const std::string& email, const std::string& password, std::string& out_mfa_ticket);
    static std::string SubmitMfaCode(const std::string& code, const std::string& ticket);

    // Connection
    bool Connect();
    void Disconnect();
    bool IsConnected() const;

    // HTTP endpoints
    std::vector<DiscordGuild>    FetchGuilds();
    std::vector<DiscordChannel>  FetchChannels(const std::string& guild_id);
    std::vector<DiscordChannel>  FetchPrivateChannels();
    std::vector<DiscordMessage>  FetchMessages(const std::string& channel_id, const std::string& before_id = "");
    bool SendDiscordMessage(const std::string& channel_id, const std::string& content);
    bool EditMessage(const std::string& channel_id, const std::string& msg_id, const std::string& new_content);
    bool DeleteMessage(const std::string& channel_id, const std::string& msg_id);
    bool AddReaction(const std::string& channel_id, const std::string& msg_id, const std::string& emoji);
    bool SendAttachment(const std::string& channel_id, const std::string& filepath);
    std::vector<unsigned char> DownloadFile(const std::string& urlStr);

    // Callbacks
    void SetOnMessageCallback(std::function<void(const DiscordMessage&)> cb);
    void SetOnConnectedCallback(std::function<void()> cb);

    // Voice
    bool JoinVoiceChannel(const std::string& guild_id, const std::string& channel_id);
    void LeaveVoiceChannel(const std::string& guild_id);
    std::vector<VoiceMember> GetVoiceMembers(const std::string& channel_id = "");
    void SetVoiceState(bool muted, bool deafened);
    void SetAudioDevices(int inputIdx, int outputIdx);
    void SubscribeToGuild(const std::string& guildId);

    // Public state (read by GUI)
    VoiceConnection          m_VoiceConn;
    std::vector<VoiceMember> m_VoiceMembers;
    std::mutex               m_VoiceMutex;

private:
    // Gateway state
    std::string         m_Token;
    std::string         m_UserId;
    std::string         m_DisplayName;
    std::string         m_AvatarHash;
    std::string         m_SessionId;
    std::string         m_VoiceSessionId;
    int                 m_HeartbeatInterval = 41250;
    int                 m_SequenceNumber    = 0;

    std::atomic<bool>   m_Connected;
    std::atomic<bool>   m_RunHeartbeat;
    std::atomic<bool>   m_VoiceReady;

    void*               m_hWebSocket = nullptr;
    std::mutex          m_WsMutex;

    std::thread         m_WsThread;
    std::thread         m_HeartbeatThread;

    std::function<void(const DiscordMessage&)> m_MessageCallback;
    std::function<void()>                      m_ConnectedCallback;

    // Internal methods
    void WebSocketLoop();
    void HeartbeatLoop();
    void VoiceLoop(std::string endpoint, std::string token, std::string guildId, std::string sessionId, std::string userId);
    void SendThreadLoop();
    void QueueWsMessage(const std::string& message);
    void AudioCaptureLoop(); // Microphone thread
    void SendIdentify(void* hWebSocket);
    std::string HttpRequest(const std::string& method, const std::string& path, const std::string& body = "");
    void ParseJsonMessage(const nlohmann::json& item, DiscordMessage& dmsg);
    void ParseVoiceStateUpdate(const nlohmann::json& d);

    std::queue<std::string> m_WsSendQueue;
    std::mutex              m_SendMutex;
    std::condition_variable m_SendCv;
    std::thread             m_SendThread;
    std::atomic<bool>       m_SendThreadRunning;
};
