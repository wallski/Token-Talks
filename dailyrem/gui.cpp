#define NOMINMAX
#include "gui.h"
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_impl_win32.h"
#include "vendor/imgui/imgui_impl_dx11.h"
#include "discord_client.h"
#include "account.h"
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#include <tchar.h>
#include <vector>
#include <mutex>
#include <map>
#include <commdlg.h>
#include <fstream>
#include <shellapi.h>
#include <mmeapi.h>
#define SODIUM_STATIC
#include "vendor/include/sodium.h"
#include "vendor/include/opus.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcontainer.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"
#include "vendor/nlohmann/json.hpp"


// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// D3D11 data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
extern void DebugLog(const std::string& msg);
static DiscordClient gc;
static std::vector<DiscordGuild> g_Guilds;
static std::vector<DiscordChannel> g_Channels;
static std::vector<DiscordMessage> g_Messages;
static std::string g_SelectedGuildId;
static std::string g_SelectedChannelId;
static std::mutex g_DataMutex;
static std::mutex g_ChatMutex;
static char g_InputBuffer[1024] = "";

struct AppSettings {
    int theme = 0;
    bool show_private_channels = true;
    char font_path[260] = {0};
    int input_device = 0;
    int output_device = 0;
    float input_volume = 1.0f;
    float output_volume = 1.0f;
};
static AppSettings g_Settings;
static bool g_ShowSettings = false;
static std::string g_EditingMessageId = "";
static char g_EditBuffer[1024] = "";
static std::string g_ActiveVoiceChannelId = "";
static std::string g_ActiveVoiceChannelName = "";
static std::string g_ActiveVoiceGuildId = "";

static std::string g_ReplyToId = "";
static std::string g_ReplyToAuthor = "";
static std::string g_ReplyToContent = "";
static bool g_ShowCallView = false;
static bool g_IsMuted = false;
static bool g_IsDeafened = false;
static std::string g_LightboxUrl = "";
static std::vector<std::string> g_ComposeFiles;
static bool g_ComposeOpen = false;
static bool g_IsFetchingMessages = false;
static bool g_RefocusInput = false;
static bool g_WasAtTop = false;       // edge-detect for infinite scroll
static std::string g_RestoreMsgId = ""; // exactly anchor the scroll when prepending
static bool g_IncomingCall = false;
static std::string g_IncomingCallChannelId = "";
static std::string g_IncomingCallUserName = "";

static std::vector<std::string> g_InputDevices;
static std::vector<std::string> g_OutputDevices;
static bool g_DevicesRefreshed = false;

void RefreshAudioDevices() {
    g_InputDevices.clear();
    int numInput = waveInGetNumDevs();
    for (int i = 0; i < numInput; ++i) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            g_InputDevices.push_back(caps.szPname);
    }
    if (g_InputDevices.empty()) g_InputDevices.push_back("No Devices Found");

    g_OutputDevices.clear();
    int numOutput = waveOutGetNumDevs();
    for (int i = 0; i < numOutput; ++i) {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            g_OutputDevices.push_back(caps.szPname);
    }
    if (g_OutputDevices.empty()) g_OutputDevices.push_back("No Devices Found");
    g_DevicesRefreshed = true;
}

void LoadSettings() {
    std::ifstream file("settings.json");
    if (file) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        try {
            auto j = nlohmann::json::parse(content);
            if (j.contains("theme")) g_Settings.theme = j["theme"].get<int>();
            if (j.contains("show_private_channels")) g_Settings.show_private_channels = j["show_private_channels"].get<bool>();
            if (j.contains("font_path")) {
                std::string fp = j["font_path"].get<std::string>();
                strncpy_s(g_Settings.font_path, fp.c_str(), _TRUNCATE);
            }
            if (j.contains("input_device")) g_Settings.input_device = j["input_device"].get<int>();
            if (j.contains("output_device")) g_Settings.output_device = j["output_device"].get<int>();
            if (j.contains("input_volume")) g_Settings.input_volume = j["input_volume"].get<float>();
            if (j.contains("output_volume")) g_Settings.output_volume = j["output_volume"].get<float>();
        } catch(...) {}
    }
}

void SaveSettings() {
    nlohmann::json j;
    j["theme"] = g_Settings.theme;
    j["show_private_channels"] = g_Settings.show_private_channels;
    j["font_path"] = std::string(g_Settings.font_path);
    j["input_device"] = g_Settings.input_device;
    j["output_device"] = g_Settings.output_device;
    j["input_volume"] = g_Settings.input_volume;
    j["output_volume"] = g_Settings.output_volume;
    std::ofstream file("settings.json");
    file << j.dump(4);
}

void ApplyTheme(int theme) {
    ImGuiStyle& style = ImGui::GetStyle();
    if (theme == 3) {
        ImGui::StyleColorsLight();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.7f, 0.7f, 0.7f, 1.00f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.6f, 0.6f, 0.6f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.5f, 0.5f, 0.5f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    } else {
        ImGui::StyleColorsDark();
        style.Colors[ImGuiCol_Text] = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        if (theme == 0) { // Blurple
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.19f, 0.20f, 0.23f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.35f, 0.40f, 0.94f, 1.00f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.35f, 0.81f, 1.00f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.26f, 0.65f, 1.00f);
        } else if (theme == 1) { // Midnight
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.2f, 0.2f, 0.2f, 1.00f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.3f, 0.3f, 1.00f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.4f, 0.4f, 0.4f, 1.00f);
        } else if (theme == 2) { // Ruby
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.05f, 0.05f, 1.00f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.06f, 0.06f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.6f, 0.1f, 0.1f, 1.00f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.7f, 0.15f, 0.15f, 1.00f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.8f, 0.2f, 0.2f, 1.00f);
        } else if (theme == 4) { // Amethyst
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.05f, 0.12f, 1.00f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.06f, 0.15f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.4f, 0.2f, 0.7f, 1.00f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.5f, 0.25f, 0.85f, 1.00f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.6f, 0.3f, 0.95f, 1.00f);
        }
    }
    
    style.FramePadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.FrameRounding = 6.0f;
    style.WindowRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;

    // Remove blue outline from keyboard/tab navigation
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);
}

struct HTTPTexture {
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0;
    int height = 0;
    std::vector<ID3D11ShaderResourceView*> frames;
    std::vector<int> delays;
    int total_duration = 0;
};

struct VideoTexture {
    ID3D11ShaderResourceView* view = nullptr;
    std::string url;
    bool is_playing = false;
    double duration = 0;
    double current_time = 0;
    int width = 0, height = 0;
};
static std::map<std::string, VideoTexture> g_Videos;
static std::mutex g_VideoMutex;
static std::map<std::string, HTTPTexture> g_Textures;
static std::mutex g_TextureMutex;
void RequestVideo(const std::string& url) {
    if (url.empty()) return;
    std::thread([url]() {
        auto bytes = gc.DownloadFile(url);
        if (bytes.empty()) return;
        
        std::string tempPath = "temp_video_" + std::to_string(GetTickCount()) + "_" + std::to_string(rand()) + ".mp4";
        std::ofstream ofs(tempPath, std::ios::binary);
        ofs.write((char*)bytes.data(), bytes.size());
        ofs.close();

        std::wstring wPath(tempPath.begin(), tempPath.end());
        IMFSourceReader* pReader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(wPath.c_str(), nullptr, &pReader))) return;

        // Set output to RGB32
        IMFMediaType* pType = nullptr;
        MFCreateMediaType(&pType);
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        pType->Release();

        // Read first frame
        DWORD streamIndex, flags;
        LONGLONG timestamp;
        IMFSample* pSample = nullptr;
        if (SUCCEEDED(pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &pSample)) && pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            pSample->ConvertToContiguousBuffer(&pBuffer);
            if (pBuffer) {
                BYTE* pData = nullptr;
                DWORD len = 0;
                pBuffer->Lock(&pData, nullptr, &len);
                
                // Get width/height
                IMFMediaType* pCurrentType = nullptr;
                pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType);
                UINT32 w, h;
                MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &w, &h);
                pCurrentType->Release();

                // Create Texture
                D3D11_TEXTURE2D_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // WMF RGB32 is BGRA
                desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA sub;
                sub.pSysMem = pData; sub.SysMemPitch = w * 4;
                
                ID3D11Texture2D* pTex = nullptr;
                ID3D11ShaderResourceView* pSrv = nullptr;
                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &pTex))) {
                    g_pd3dDevice->CreateShaderResourceView(pTex, nullptr, &pSrv);
                    pTex->Release();
                }
                
                pBuffer->Unlock();
                pBuffer->Release();

                if (pSrv) {
                    std::lock_guard<std::mutex> lock(g_VideoMutex);
                    g_Videos[url] = { pSrv, url, false, 0, 0, (int)w, (int)h };
                }
            }
            pSample->Release();
        }
        pReader->Release();
        DeleteFileA(tempPath.c_str());
    }).detach();
}

void RequestTexture(const std::string& url) {
    if (url.empty()) return;
    std::thread([url]() {
        auto bytes = gc.DownloadFile(url);
        if (bytes.empty()) return;

        bool isGif = (url.size() >= 4 && url.substr(url.size() - 4) == ".gif");
        
        int w, h, n;
        std::vector<ID3D11ShaderResourceView*> frames;
        std::vector<int> delays;
        int total_duration = 0;

        if (isGif) {
            int* stbi_delays = nullptr;
            int frames_count = 0;
            int channels = 4;
            unsigned char* data = stbi_load_gif_from_memory(bytes.data(), (int)bytes.size(), &stbi_delays, &w, &h, &frames_count, &channels, 4);
            
            if (data && frames_count > 0) {
                int frame_size = w * h * 4;
                for (int i = 0; i < frames_count; ++i) {
                    D3D11_TEXTURE2D_DESC desc;
                    ZeroMemory(&desc, sizeof(desc));
                    desc.Width = w;
                    desc.Height = h;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                    D3D11_SUBRESOURCE_DATA subResource;
                    subResource.pSysMem = data + (i * frame_size);
                    subResource.SysMemPitch = desc.Width * 4;
                    subResource.SysMemSlicePitch = 0;

                    ID3D11Texture2D* pTexture = nullptr;
                    ID3D11ShaderResourceView* pSrv = nullptr;

                    if (g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture) == S_OK) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                        ZeroMemory(&srvDesc, sizeof(srvDesc));
                        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = 1;
                        g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSrv);
                        pTexture->Release();
                        if (pSrv) {
                            frames.push_back(pSrv);
                            int d = stbi_delays[i];
                            if (d == 0) d = 100; // Fallback 10fps
                            delays.push_back(d);
                            total_duration += d;
                        }
                    }
                }
                stbi_image_free(data);
                if (stbi_delays) stbi_image_free(stbi_delays);
            }
        } else {
            unsigned char* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
            if (data) {
                D3D11_TEXTURE2D_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA subResource;
                subResource.pSysMem = data; subResource.SysMemPitch = desc.Width * 4;
                ID3D11Texture2D* pTexture = nullptr;
                ID3D11ShaderResourceView* pSrv = nullptr;
                if (g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture) == S_OK) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                    ZeroMemory(&srvDesc, sizeof(srvDesc));
                    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSrv);
                    pTexture->Release();
                }
                stbi_image_free(data);
                if (pSrv) frames.push_back(pSrv);
            }
        }

        if (!frames.empty()) {
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            g_Textures[url] = { frames[0], w, h, frames, delays, total_duration };
        }
    }).detach();
}

std::string OpenMediaFileDialog() {
    OPENFILENAMEA ofn;
    char szFile[260] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Media\0*.PNG;*.JPG;*.JPEG;*.GIF;*.MP4\0All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn) == TRUE) {
        return szFile;
    }
    return "";
}

static std::string MakeAvatarUrl(const std::string& uid, const std::string& hash) {
    if (hash.empty() || uid.empty()) return "";
    return "https://cdn.discordapp.com/avatars/" + uid + "/" + hash + ".png?size=40";
}
static std::string MakeGuildIconUrl(const std::string& gid, const std::string& hash) {
    if (hash.empty() || gid.empty()) return "";
    return "https://cdn.discordapp.com/icons/" + gid + "/" + hash + ".png?size=64";
}
static void DrawAvatarCircle(ImDrawList* dl, ImVec2 c, float r,
                              const std::string& uid, const std::string& hash, const std::string& name) {
    std::string url = MakeAvatarUrl(uid, hash);
    bool drawn = false;
    if (!url.empty()) {
        bool need = false; HTTPTexture tx = {};
        { std::lock_guard<std::mutex> lk(g_TextureMutex);
          if (!g_Textures.count(url)) { g_Textures[url] = {}; need = true; } else tx = g_Textures[url]; }
        if (need) RequestTexture(url);
        if (tx.view) { dl->AddImageRounded((void*)tx.view, {c.x-r,c.y-r}, {c.x+r,c.y+r}, {0,0}, {1,1}, IM_COL32_WHITE, r); drawn = true; }
    }
    if (!drawn) {
        uint32_t h = 5381; for (char ch : uid) h = ((h<<5)+h)+(unsigned char)ch;
        static ImU32 pal[] = { IM_COL32(88,101,242,255), IM_COL32(87,242,135,255), IM_COL32(254,231,92,255),
                                IM_COL32(235,69,158,255), IM_COL32(0,185,255,255),  IM_COL32(250,119,0,255) };
        dl->AddCircleFilled(c, r, pal[h%6]);
        if (!name.empty()) {
            char s[2] = {(char)toupper((unsigned char)name[0]), 0};
            ImVec2 ts = ImGui::CalcTextSize(s);
            dl->AddText({c.x-ts.x*.5f, c.y-ts.y*.5f}, IM_COL32_WHITE, s);
        }
    }
}

static std::vector<Account> g_TokenAccounts;
static std::vector<Account> g_MailAccounts;
static bool g_IsLoggedIn = false;
static std::string g_TokenError = "";
static std::string g_MailError = "";
static std::string g_MfaTicket = "";
static bool g_ShowMfaModal = false;
static char g_MfaCode[16] = "";
static char g_PendingMailName[128] = "";

void LoadAccountsGUI() {
    LoadTokenAccounts(g_TokenAccounts);
    LoadMailAccounts(g_MailAccounts);
}

void RefreshGuilds() {
    auto guilds = gc.FetchGuilds();
    std::lock_guard<std::mutex> lock(g_DataMutex);
    g_Guilds = guilds;
    g_Channels.clear();
    g_SelectedGuildId = "";
    g_SelectedChannelId = "";
}

void RefreshPrivateChannels() {
    auto channels = gc.FetchPrivateChannels();
    std::lock_guard<std::mutex> lock(g_DataMutex);
    g_SelectedGuildId = "";
    g_Channels = channels;
    g_SelectedChannelId = "";
}

void RefreshChannels(const std::string& guildId) {
    auto channels = gc.FetchChannels(guildId);
    std::lock_guard<std::mutex> lock(g_DataMutex);
    g_SelectedGuildId = guildId;
    g_Channels = channels;
    g_SelectedChannelId = "";
}

void RefreshMessages(const std::string& channelId) {
    auto msgs = gc.FetchMessages(channelId);
    std::lock_guard<std::mutex> lock(g_DataMutex);
    if (g_SelectedChannelId == channelId) {
        std::lock_guard<std::mutex> chatLock(g_ChatMutex);
        g_Messages = msgs;
    }
}

void OnDiscordMessage(const DiscordMessage& msg) {
    std::lock_guard<std::mutex> lock(g_ChatMutex);
    if (!g_SelectedChannelId.empty()) { // Could filter by channel later
        g_Messages.push_back(msg);
    }
}

void OnIncomingCall(const std::string& cid, const std::string& name) {
    if (name == "STOP") {
        g_IncomingCall = false;
        return;
    }
    g_IncomingCall = true;
    g_IncomingCallChannelId = cid;
    g_IncomingCallUserName = name;
}

void DrawDashboard() {
    ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    ImVec2 vp_pos = ImGui::GetMainViewport()->Pos;

    // 100% Seamless full-screen window
    ImGui::SetNextWindowPos(vp_pos);
    ImGui::SetNextWindowSize(vp_size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(54, 57, 63, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    
    ImGui::Begin("Dashboard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // --- LEFT SIDEBAR (SAVED ACCOUNTS) ---
    float sidebarW = 240.0f;
    ImGui::SetCursorPos({20, 40});
    ImGui::BeginGroup();

    // Debug: Log session state once it's captured
    static bool loggedSession = false;
    if (!loggedSession && !gc.GetToken().empty() && !gc.GetSessionId().empty()) {
        DebugLog("[DASHBOARD] Identity Confirmed - Session: " + gc.GetSessionId() + " User: " + gc.GetUserId());
        loggedSession = true;
    }

    ImGui::TextDisabled("SAVED ACCOUNTS");
    ImGui::Dummy({0, 10});

    auto drawAccounts = [&](std::vector<Account>& list, bool isMail) {
        for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID((isMail ? 20000 : 10000) + (int)i);
            if (ImGui::Button(list[i].name.c_str(), {sidebarW - 50, 40})) {
                gc.SetToken(list[i].token);
                gc.Connect();
                g_IsLoggedIn = true;
                std::thread([]() { RefreshGuilds(); RefreshPrivateChannels(); }).detach();
            }
            ImGui::SameLine();
            if (ImGui::Button("X", {32, 40})) {
                list.erase(list.begin() + i);
                if (isMail) SaveMailAccounts(list); else SaveTokenAccounts(list);
                ImGui::PopID(); break;
            }
            ImGui::PopID();
        }
    };
    drawAccounts(g_TokenAccounts, false);
    drawAccounts(g_MailAccounts, true);
    ImGui::EndGroup();

    // --- CENTER LOGIN SECTION ---
    float panelW = 400.0f;
    float startX = (vp_size.x - panelW) * 0.5f;
    float startY = vp_size.y * 0.20f;

    ImGui::SetCursorPos({startX, startY});
    ImGui::BeginGroup();

    // Title text
    const char* title = "Token-Talks";
    ImGui::SetCursorPosX(startX + (panelW - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextColored(ImVec4(0.35f, 0.40f, 0.94f, 1.0f), title);
    ImGui::Dummy({0, 40});

    // Clip the TabBar underline
    ImVec2 clipMin = { startX, vp_pos.y };
    ImVec2 clipMax = { startX + panelW, vp_pos.y + vp_size.y };
    ImGui::PushClipRect(clipMin, clipMax, true);
    ImGui::PushItemWidth(panelW);

    if (ImGui::BeginTabBar("LoginTabs")) {
        if (ImGui::BeginTabItem("Token Login")) {
            static char tName[128] = "", tTok[256] = "";
            ImGui::Dummy({0, 10});
            ImGui::SetCursorPosX(startX); ImGui::TextDisabled("DISPLAY NAME");
            ImGui::SetCursorPosX(startX); ImGui::InputText("##tn", tName, 128);
            ImGui::SetCursorPosX(startX); ImGui::TextDisabled("TOKEN");
            ImGui::SetCursorPosX(startX); ImGui::InputText("##tt", tTok, 256, ImGuiInputTextFlags_Password);
            ImGui::Dummy({0, 20});
            ImGui::SetCursorPosX(startX);
            if (ImGui::Button("Log In with Token", {panelW, 45})) {
                if (strlen(tName) > 0 && DiscordClient::ValidateToken(tTok)) {
                    g_TokenAccounts.push_back({tName, tTok, AccountType::TOKEN});
                    SaveTokenAccounts(g_TokenAccounts);
                    gc.SetToken(tTok); gc.Connect(); g_IsLoggedIn = true;
                    std::thread([]() { RefreshGuilds(); RefreshPrivateChannels(); }).detach();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Email Login")) {
            static char mName[128] = "", mEmail[256] = "", mPass[256] = "";
            ImGui::Dummy({0, 10});
            ImGui::SetCursorPosX(startX); ImGui::TextDisabled("NAME");
            ImGui::SetCursorPosX(startX); ImGui::InputText("##mn", mName, 128);
            ImGui::SetCursorPosX(startX); ImGui::TextDisabled("EMAIL");
            ImGui::SetCursorPosX(startX); ImGui::InputText("##me", mEmail, 256);
            ImGui::SetCursorPosX(startX); ImGui::TextDisabled("PASSWORD");
            ImGui::SetCursorPosX(startX); ImGui::InputText("##mp", mPass, 256, ImGuiInputTextFlags_Password);
            ImGui::Dummy({0, 20});
            ImGui::SetCursorPosX(startX);
            if (ImGui::Button("Log In with Email", {panelW, 45})) {
                if (strlen(mName) > 0 && strlen(mEmail) > 0 && strlen(mPass) > 0) {
                    std::string name = mName, email = mEmail, pass = mPass;
                    strncpy_s(g_PendingMailName, name.c_str(), _TRUNCATE);
                    std::thread([name, email, pass]() {
                        std::string mfa_ticket;
                        std::string token = DiscordClient::LoginWithCredentials(email, pass, mfa_ticket);
                        if (!token.empty()) {
                            g_MailAccounts.push_back({name, token, AccountType::EMAIL});
                            SaveMailAccounts(g_MailAccounts);
                            gc.SetToken(token); gc.Connect(); g_IsLoggedIn = true;
                            std::thread([]() { RefreshGuilds(); RefreshPrivateChannels(); }).detach();
                        } else if (!mfa_ticket.empty()) {
                            g_MfaTicket = mfa_ticket; g_ShowMfaModal = true;
                        }
                    }).detach();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::PopClipRect();

    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void DrawMfaModal() {
    if (g_ShowMfaModal) {
        ImGui::OpenPopup("MFA Required");
    }
    if (ImGui::BeginPopupModal("MFA Required", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter MFA Code:");
        static char mfaBuf[128] = "";
        ImGui::InputText("##code", mfaBuf, sizeof(mfaBuf));
        if (ImGui::Button("Confirm")) {
            g_ShowMfaModal = false;
        }
        ImGui::EndPopup();
    }
}

void DrawIncomingCallOverlay() {
    if (!g_IncomingCall) return;

    ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    float boxW = 320.0f, boxH = 160.0f;
    ImGui::SetNextWindowPos({(vp_size.x - boxW) * 0.5f, 100});
    ImGui::SetNextWindowSize({boxW, boxH});
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.16f, 0.18f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::Begin("IncomingCallPopup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetCursorPosY(25);
    std::string txt = "INCOMING CALL";
    float txtW = ImGui::CalcTextSize(txt.c_str()).x;
    ImGui::SetCursorPosX((boxW - txtW) * 0.5f);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s", txt.c_str());

    ImGui::SetCursorPosY(55);
    std::string userTxt = g_IncomingCallUserName;
    float userW = ImGui::CalcTextSize(userTxt.c_str()).x;
    ImGui::SetCursorPosX((boxW - userW) * 0.5f);
    ImGui::TextUnformatted(userTxt.c_str());

    ImGui::SetCursorPosY(100);
    ImGui::SetCursorPosX((boxW - 220) * 0.5f);
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("ACCEPT", {100, 38})) {
        gc.AcceptCall(g_IncomingCallChannelId);
        g_IncomingCall = false;
        g_ShowCallView = true;
        g_ActiveVoiceChannelId = g_IncomingCallChannelId;
        g_ActiveVoiceChannelName = "Private Call";
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 20);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button("DECLINE", {100, 38})) {
        gc.DeclineCall(g_IncomingCallChannelId);
        g_IncomingCall = false;
    }
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawCallView() {
    ImGui::BeginChild("CallContainer", ImVec2(0, 0), false);
    
    // Background styled area
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + ImGui::GetContentRegionAvail().y);
    drawList->AddRectFilledMultiColor(p0, p1, ImColor(30, 31, 34), ImColor(30, 31, 34), ImColor(20, 21, 23), ImColor(20, 21, 23));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
    if (!gc.m_VoiceConn.m_Ready)
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "RTC CONNECTING... / %s", g_ActiveVoiceChannelName.c_str());
    else
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "VOICE CONNECTED / %s", g_ActiveVoiceChannelName.c_str());
    
    // Participants Grid
    ImGui::SetCursorPos(ImVec2(50, 100));
    ImGui::BeginGroup();
    {
        auto members = gc.GetVoiceMembers(g_ActiveVoiceChannelId);
        if (members.empty()) {
            ImGui::SetCursorPos(ImVec2(100, 200));
            ImGui::TextDisabled("Waiting for members...");
        }
        for (int i = 0; i < (int)members.size(); ++i) {
            ImGui::BeginChild((std::string("Member_") + std::to_string(i)).c_str(), ImVec2(220, 260), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
            ImDrawList* d = ImGui::GetWindowDrawList();
            ImVec2 cp = ImGui::GetCursorScreenPos();
            
            // Modern Discord-like card
            d->AddRectFilled(cp, ImVec2(cp.x + 220, cp.y + 260), ImColor(43, 45, 49), 12.0f);
            if (members[i].m_IsSpeaking) // Real speaking detection placeholder
                d->AddRect(cp, ImVec2(cp.x + 220, cp.y + 260), ImColor(50, 160, 50, 255), 12.0f, 0, 2.0f);
            
            float circleX = cp.x + 110;
            float circleY = cp.y + 100;
            d->AddCircleFilled(ImVec2(circleX, circleY), 70, ImColor(30, 31, 34));
            
            // Smooth speaking ring
            bool isMe = (members[i].m_Id == gc.GetUserId());
            float pulse = (sinf((float)ImGui::GetTime() * 8.0f) * 0.5f + 0.5f);
            if (members[i].m_IsSpeaking || (isMe && !g_IsMuted && pulse > 0.3f)) {
                d->AddCircle(ImVec2(circleX, circleY), 72 + (pulse * 3.0f), ImColor(50, 160, 50, (int)(200 * pulse)), 0, 3.0f);
            }
            
            ImGui::SetCursorPosY(185);
            // prefer display name over raw username
            std::string nameLabel = members[i].m_DisplayName.empty() ? members[i].m_Username : members[i].m_DisplayName;
            if (isMe) nameLabel += " (You)";
            ImVec2 nSize = ImGui::CalcTextSize(nameLabel.c_str());
            ImGui::SetCursorPosX(110 - nSize.x * 0.5f);
            ImGui::TextUnformatted(nameLabel.c_str());

            // Draw avatar inside circle
            if (!members[i].m_AvatarHash.empty()) {
                std::string avUrl = MakeAvatarUrl(members[i].m_Id, members[i].m_AvatarHash);
                DrawAvatarCircle(d, ImVec2(circleX, circleY), 68.f, members[i].m_Id, members[i].m_AvatarHash, nameLabel);
            } else {
                DrawAvatarCircle(d, ImVec2(circleX, circleY), 68.f, members[i].m_Id, "", nameLabel);
            }

            if (members[i].m_IsMuted || members[i].m_IsDeafened) {
                ImGui::SetCursorPosY(215);
                const char* mTxt = members[i].m_IsDeafened ? "DEAFENED" : "MUTED";
                ImVec2 mSize = ImGui::CalcTextSize(mTxt);
                ImGui::SetCursorPosX(110 - mSize.x * 0.5f);
                ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), mTxt);
            }
            ImGui::EndChild();
            if (i < (int)members.size() - 1) ImGui::SameLine(0, 30);
        }
    }
    ImGui::EndGroup();

    // Floating Call Toolbar
    float toolbarW = 380;
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - toolbarW) * 0.5f, ImGui::GetWindowHeight() - 85));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 25.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 0.95f));
    ImGui::BeginChild("CallControls", ImVec2(toolbarW, 60), true, ImGuiWindowFlags_NoScrollbar);

    auto IconButton = [](const char* label, bool active, ImVec4 activeCol, const char* tooltip) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? activeCol : ImVec4(0.18f, 0.19f, 0.21f, 1.0f));
        ImGui::SetCursorPosY(10);
        bool pressed = ImGui::Button(label, ImVec2(80, 40));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::PopStyleColor(); ImGui::PopStyleVar();
        return pressed;
    };

    ImGui::SetCursorPosX(15);
    if (IconButton(g_IsMuted ? "MIC OFF" : "MIC ON", g_IsMuted, ImVec4(0.9f,0.3f,0.3f,1.f), "Toggle Mic")) { g_IsMuted=!g_IsMuted; gc.SetVoiceState(g_IsMuted,g_IsDeafened); }
    ImGui::SameLine(0,10);
    if (IconButton(g_IsDeafened ? "DEAF ON" : "DEAF OFF", g_IsDeafened, ImVec4(0.9f,0.3f,0.3f,1.f), "Toggle Deafen")) { g_IsDeafened=!g_IsDeafened; gc.SetVoiceState(g_IsMuted,g_IsDeafened); }
    ImGui::SameLine(0,10);
    if (IconButton("CHAT", false, {0,0,0,0}, "Back to Chat")) g_ShowCallView = false;
    ImGui::SameLine(0,10);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f,0.2f,0.2f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f,0.1f,0.1f,1.f));
    ImGui::SetCursorPosY(10);
    if (ImGui::Button("DISCONNECT", ImVec2(90,40))) { gc.LeaveVoiceChannel(g_ActiveVoiceGuildId); g_ActiveVoiceChannelId=""; g_ShowCallView=false; }
    ImGui::PopStyleColor(2);
    ImGui::EndChild();  // CallControls
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndChild();  // CallContainer  <-- FIX: was missing, caused ImGui assert
}

void DrawMainApp() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
    ImGui::Begin("MainApp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f,0.122f,0.133f,1.f));
    ImGui::BeginChild("##topbar", ImVec2(0,40), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos({10,10});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.57f,0.62f,1.f));
    std::string topLabel = gc.GetUserName().empty() ? gc.GetUserId() : gc.GetUserName();
    ImGui::TextUnformatted(("Token-Talks  |  " + topLabel).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth()-190);
    if (ImGui::Button("Settings", {88,24})) { g_ShowSettings=!g_ShowSettings; if(g_ShowSettings)RefreshAudioDevices(); }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.15f,0.15f,1.f));
    if (ImGui::Button("Logout", {78,24})) { std::thread([](){gc.Disconnect();}).detach(); g_IsLoggedIn=false; g_ShowSettings=false; }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    
    if (g_ShowSettings) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.18f, 0.19f, 0.22f, 1.0f));
        ImGui::Begin("SettingsWindow", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        
        static int selected_tab = 0;
        
        // Sidebar
        ImGui::BeginChild("SettingsSidebar", ImVec2(260, 0), false);
        ImGui::SetCursorPos({20, 60});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.60f, 1.0f));
        ImGui::SetWindowFontScale(0.9f);
        ImGui::Text("USER SETTINGS");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 4});
        
        auto drawTab = [&](const char* name, int id) {
            ImGui::SetCursorPosX(16);
            ImGui::PushStyleColor(ImGuiCol_Header, selected_tab == id ? ImVec4(0.25f, 0.27f, 0.32f, 1.f) : ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.24f, 0.28f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
            if (ImGui::Selectable(name, selected_tab == id, 0, {228, 32})) selected_tab = id;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        };
        
        drawTab(" Appearance", 0);
        drawTab(" Voice & Audio", 1);
        drawTab(" Privacy & Safety", 2);
        ImGui::EndChild();
        
        ImGui::SameLine(0, 0);
        
        // Main Content Area
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.21f, 0.22f, 0.25f, 1.0f));
        ImGui::BeginChild("SettingsContent", ImVec2(0, 0), false);
        ImGui::SetCursorPos({40, 60});
        ImGui::BeginGroup();
        
        if (selected_tab == 0) {
            ImGui::SetWindowFontScale(1.2f);
            ImGui::Text("Appearance");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Dummy({0, 20});
            
            ImGui::Text("Theme Selection");
            ImGui::PushItemWidth(300);
            if (ImGui::Combo("##Theme", &g_Settings.theme, "Blurple Dark\0Midnight\0Ruby\0Light Mode\0Amethyst\0")) {
                ApplyTheme(g_Settings.theme);
                SaveSettings();
            }
            ImGui::PopItemWidth();
            
            ImGui::Dummy({0, 10});
            if (ImGui::Checkbox("Show Private / Locked Channels", &g_Settings.show_private_channels)) SaveSettings();

            ImGui::Dummy({0, 20});
            ImGui::Text("Custom Font (Requires Restart)");
            ImGui::PushItemWidth(300);
            ImGui::InputText("##FontPath", g_Settings.font_path, sizeof(g_Settings.font_path));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string p = OpenMediaFileDialog();
                if(!p.empty()){
                    strncpy_s(g_Settings.font_path, p.c_str(), _TRUNCATE);
                    SaveSettings();
                }
            }
        } 
        else if (selected_tab == 1) {
            ImGui::SetWindowFontScale(1.2f);
            ImGui::Text("Voice & Audio");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Dummy({0, 20});
            
            ImGui::Text("Input Device");
            ImGui::PushItemWidth(400);
            if (ImGui::BeginCombo("##InDev", g_InputDevices[g_Settings.input_device < (int)g_InputDevices.size() ? g_Settings.input_device : 0].c_str())) {
                for (int i = 0; i < (int)g_InputDevices.size(); ++i) {
                    if (ImGui::Selectable(g_InputDevices[i].c_str(), g_Settings.input_device == i)) {
                        g_Settings.input_device = i; SaveSettings();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Input Volume", &g_Settings.input_volume, 0.0f, 2.0f, "%.1fx");
            
            ImGui::Dummy({0, 20});
            ImGui::Text("Output Device");
            if (ImGui::BeginCombo("##OutDev", g_OutputDevices[g_Settings.output_device < (int)g_OutputDevices.size() ? g_Settings.output_device : 0].c_str())) {
                for (int i = 0; i < (int)g_OutputDevices.size(); ++i) {
                    if (ImGui::Selectable(g_OutputDevices[i].c_str(), g_Settings.output_device == i)) {
                        g_Settings.output_device = i; SaveSettings();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Output Volume", &g_Settings.output_volume, 0.0f, 2.0f, "%.1fx");
            ImGui::PopItemWidth();
            ImGui::Dummy({0, 10});
            if (ImGui::Button("Refresh Devices")) RefreshAudioDevices();
        } 
        else if (selected_tab == 2) {
            ImGui::SetWindowFontScale(1.2f);
            ImGui::Text("Privacy & Safety");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Dummy({0, 20});
            
            static bool allowDms = true;
            static bool filterMessages = true;
            static bool showActivity = true;

            ImGui::Checkbox("Allow Direct Messages from server members", &allowDms);
            ImGui::TextDisabled(" Disable this to block DMs from non-friends.");
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Checkbox("Filter potentially explicit content in messages", &filterMessages);
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Checkbox("Display current activity as a status message", &showActivity);
        }
        ImGui::EndGroup();
        
        // Close Button
        ImGui::SetCursorPos({ImGui::GetWindowWidth() - 100, 60});
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.35f,1.f));
        if (ImGui::Button("X", {40, 40})) g_ShowSettings = false;
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
        ImGui::TextDisabled("ESC");
        ImGui::EndGroup();
        
        ImGui::EndChild();         // SettingsContent
        ImGui::PopStyleColor();    // SettingsContent bg
        
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_ShowSettings = false;
        ImGui::End();              // SettingsWindow
        ImGui::PopStyleColor();    // SettingsWindow WindowBg
        ImGui::End();              // MainApp
        return;
    }

    // ═══ 3-COLUMN LAYOUT ═══════════════════════════════════════════
    const float RAIL_W=72.f, CHAN_W=220.f;
    ImVec4 acV=ImGui::GetStyle().Colors[ImGuiCol_Button];
    ImU32  accent=ImGui::ColorConvertFloat4ToU32(acV);

    // ── SERVER RAIL ──────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f,0.122f,0.133f,1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,8));
    ImGui::BeginChild("##rail",{RAIL_W,0},false,ImGuiWindowFlags_NoScrollbar);
    ImDrawList* rDL=ImGui::GetWindowDrawList();
    auto serverBtn=[&](const std::string& id,const std::string& name,const std::string& iconH,bool active)->bool{
        ImVec2 sp=ImGui::GetCursorScreenPos();
        float cx=sp.x+RAIL_W*.5f,cy=sp.y+24.f,r=active?14.f:20.f;
        if(active) rDL->AddRectFilled({sp.x,cy-16},{sp.x+4,cy+16},accent,2.f);
        ImGui::InvisibleButton(("##s"+id).c_str(),{RAIL_W,48});
        bool hov=ImGui::IsItemHovered(); if(hov&&!active) r=16.f;
        std::string iurl=MakeGuildIconUrl(id,iconH);
        bool drw=false;
        if(!iurl.empty()){
            bool need=false; HTTPTexture tx={};
            {std::lock_guard<std::mutex> lk(g_TextureMutex);if(!g_Textures.count(iurl)){g_Textures[iurl]={};need=true;}else tx=g_Textures[iurl];}
            if(need)RequestTexture(iurl);
            if(tx.view){rDL->AddImageRounded((void*)tx.view,{cx-r,cy-r},{cx+r,cy+r},{0,0},{1,1},IM_COL32_WHITE,r);drw=true;}
        }
        if(!drw){
            ImU32 bg=active?accent:(hov?IM_COL32(88,101,242,255):IM_COL32(54,57,62,255));
            rDL->AddCircleFilled({cx,cy},r,bg);
            if(!name.empty()){char s[2]={(char)toupper((unsigned char)name[0]),0};ImVec2 ts=ImGui::CalcTextSize(s);rDL->AddText({cx-ts.x*.5f,cy-ts.y*.5f},IM_COL32_WHITE,s);}
        }
        if(hov)ImGui::SetTooltip("%s",name.c_str());
        return ImGui::IsItemClicked();
    };
    if(serverBtn("__dm__","DMs","",g_SelectedGuildId.empty())) std::thread([](){RefreshPrivateChannels();}).detach();
    {ImVec2 p=ImGui::GetCursorScreenPos();rDL->AddRectFilled({p.x+14,p.y+4},{p.x+RAIL_W-14,p.y+6},IM_COL32(80,80,85,255),1.f);ImGui::Dummy({0,14});}
    {std::lock_guard<std::mutex> lk(g_DataMutex);for(auto&g:g_Guilds)if(serverBtn(g.id,g.name,g.icon_hash,g.id==g_SelectedGuildId))std::thread([g](){RefreshChannels(g.id); gc.SubscribeToGuild(g.id);}).detach();}
    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::SameLine(0,0);

    // ── CHANNEL LIST ─────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.157f,0.169f,0.188f,1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::BeginChild("##chan",{CHAN_W,0},false);
    ImDrawList* cDL=ImGui::GetWindowDrawList();
    {
        std::string hdr=g_SelectedGuildId.empty()?"Direct Messages":"";
        {std::lock_guard<std::mutex> lk(g_DataMutex);for(auto&g:g_Guilds)if(g.id==g_SelectedGuildId){hdr=g.name;break;}}
        ImVec2 hp=ImGui::GetCursorScreenPos();
        cDL->AddRectFilled(hp,{hp.x+CHAN_W,hp.y+48},IM_COL32(32,34,37,255));
        cDL->AddLine({hp.x,hp.y+48},{hp.x+CHAN_W,hp.y+48},IM_COL32(0,0,0,80));
        ImGui::SetCursorPos({12,14});
        ImGui::PushStyleColor(ImGuiCol_Text,{1,1,1,1});ImGui::TextUnformatted(hdr.c_str());ImGui::PopStyleColor();
        ImGui::Dummy({0,16});
    }
    ImGui::BeginChild("##chsc",{0,0},false);
    {
        std::lock_guard<std::mutex> lk(g_DataMutex);
        auto chs=g_Channels;
        std::sort(chs.begin(),chs.end(),[](auto&a,auto&b){return a.position<b.position;});
        auto drawSec=[&](const char* secLbl,int tf){
            ImGui::Dummy({0,6});ImGui::SetCursorPosX(10);
            ImGui::PushStyleColor(ImGuiCol_Text,{0.50f,0.52f,0.56f,1.f});
            ImGui::TextUnformatted(secLbl);ImGui::PopStyleColor();
            for(auto&c:chs){
                if(c.type!=tf)continue;
                if(!g_Settings.show_private_channels&&c.is_locked)continue;
                bool sel=(c.id==g_SelectedChannelId);
                // ASCII prefixes — default font has no emoji glyphs
                std::string lb=(tf==2?"(vc) ":"# ")+c.name;
                if(c.is_locked)lb="[L] "+c.name;
                ImGui::PushStyleColor(ImGuiCol_Header,sel?ImVec4(0.22f,0.24f,0.30f,1.f):ImVec4(0,0,0,0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{0.18f,0.20f,0.26f,1.f});
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,{0.24f,0.26f,0.32f,1.f});
                ImGui::SetCursorPosX(8);
                if(ImGui::Selectable(lb.c_str(),sel,0,{CHAN_W-16,28})){
                    if(tf==2){
                        gc.SetAudioDevices(g_Settings.input_device,g_Settings.output_device);
                        if(gc.JoinVoiceChannel(g_SelectedGuildId,c.id)){g_ActiveVoiceChannelId=c.id;g_ActiveVoiceChannelName=c.name;g_ActiveVoiceGuildId=g_SelectedGuildId;g_ShowCallView=true;}
                    } else {
                        { std::lock_guard<std::mutex> lk(g_DataMutex); g_SelectedChannelId = c.id; }
                        std::thread([c](){RefreshMessages(c.id);}).detach();
                    }
                }
                ImGui::PopStyleColor(3);
                if(tf==2){
                    std::lock_guard<std::mutex> vl(gc.m_VoiceMutex);
                    for(auto&vm:gc.m_VoiceMembers)if(vm.m_ChannelId==c.id){
                        ImGui::SetCursorPosX(28);
                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                        ImGui::Dummy({20, 20});
                        DrawAvatarCircle(cDL, {p0.x + 10, p0.y + 10}, 10.f, vm.m_Id, vm.m_AvatarHash, vm.m_DisplayName.empty() ? vm.m_Username : vm.m_DisplayName);
                        ImGui::SameLine(54);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.67f, 0.70f, 1.0f));
                        std::string vn = vm.m_DisplayName.empty() ? vm.m_Username : vm.m_DisplayName;
                        ImGui::TextUnformatted(vn.c_str());
                        ImGui::PopStyleColor();
                    }
                }
            }
        };
        if(g_SelectedGuildId.empty()){
            // DM mode: all channels are type 1 (DM) or 3 (group DM) — show flat list
            ImGui::Dummy({0,6});ImGui::SetCursorPosX(10);
            ImGui::PushStyleColor(ImGuiCol_Text,{0.50f,0.52f,0.56f,1.f});
            ImGui::TextUnformatted("DIRECT MESSAGES");ImGui::PopStyleColor();
            for(auto&c:chs){
                bool sel=(c.id==g_SelectedChannelId);
                ImGui::PushStyleColor(ImGuiCol_Header,sel?ImVec4(0.22f,0.24f,0.30f,1.f):ImVec4(0,0,0,0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{0.18f,0.20f,0.26f,1.f});
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,{0.24f,0.26f,0.32f,1.f});
                ImGui::SetCursorPosX(8);
                if(ImGui::Selectable(c.name.c_str(),sel,0,{CHAN_W-16,28})){
                    { std::lock_guard<std::mutex> lk(g_DataMutex); g_SelectedChannelId = c.id; }
                    std::thread([c](){RefreshMessages(c.id);}).detach();
                }
                ImGui::PopStyleColor(3);
            }
        } else {
            drawSec("TEXT CHANNELS",0);
            drawSec("VOICE CHANNELS",2);
        }
    }
    ImGui::EndChild();ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::SameLine(0,0);

    // ── CONTENT ──────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.212f,0.224f,0.243f,1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::BeginChild("##content",{0,0},false,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    if(!g_ActiveVoiceChannelId.empty()&&g_ShowCallView){
        DrawCallView();
    } else {
        if(!g_ActiveVoiceChannelId.empty()){
            ImGui::PushStyleColor(ImGuiCol_ChildBg,{0.10f,0.12f,0.14f,0.95f});
            ImGui::SetCursorPos({10,6});ImGui::BeginChild("##vcpill",{ImGui::GetContentRegionAvail().x-20,36},false);
            ImGui::SetCursorPos({10,8});
            if(!gc.m_VoiceConn.m_Ready)ImGui::TextColored({0.9f,0.8f,0.1f,1.f},"(vc) Connecting... %s",g_ActiveVoiceChannelName.c_str());
            else ImGui::TextColored({0.3f,0.9f,0.4f,1.f},"(vc) Voice Active: %s",g_ActiveVoiceChannelName.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth()-148);
            if(ImGui::Button("Open Call",{138,22}))g_ShowCallView=true;
            ImGui::EndChild();ImGui::PopStyleColor();ImGui::Dummy({0,2});
        }
        {
            std::string ch_hdr="Select a channel";
            {std::lock_guard<std::mutex> lk(g_DataMutex);for(auto&c:g_Channels)if(c.id==g_SelectedChannelId){ch_hdr=(c.type==2?"(vc) ":"# ")+c.name;break;}}
            ImGui::SetCursorPosX(14);
            ImGui::PushStyleColor(ImGuiCol_Text,{0.95f,0.96f,0.98f,1.f});ImGui::TextUnformatted(ch_hdr.c_str());ImGui::PopStyleColor();

            // Add Call Button for DMs
            if (g_SelectedGuildId.empty() && !g_SelectedChannelId.empty()) {
                ImGui::SameLine(ImGui::GetWindowWidth() - 60);
                if (ImGui::Button("CALL", {40, 24})) {
                    std::string cid = g_SelectedChannelId;
                    std::thread([cid]() {
                        gc.StartCall(cid);
                    }).detach();
                    g_ActiveVoiceChannelId = g_SelectedChannelId;
                    g_ActiveVoiceChannelName = "Private Call";
                    g_ShowCallView = true;
                }
            }
            ImGui::Dummy({0,4});
        }
        // Messages — height leaves room for input bar + optional compose staging
        float inputAreaH = 54.f;
        if (g_ComposeOpen && !g_ComposeFiles.empty()) inputAreaH += 68.f;
        ImGui::BeginChild("##msgs",{0,-inputAreaH},false,ImGuiWindowFlags_NoNav);

        // Infinite scroll: trigger only on the LEADING EDGE of reaching the top (not every frame)
        float curScroll = ImGui::GetScrollY();
        bool isAtTop = (curScroll < 60.f);
        if (isAtTop && !g_WasAtTop && !g_IsFetchingMessages && !g_SelectedChannelId.empty()) {
            bool hasMessages = false; std::string firstId;
            { std::lock_guard<std::mutex> lk(g_ChatMutex); if(!g_Messages.empty()){ hasMessages=true; firstId=g_Messages[0].id; } }
            if (hasMessages) {
                g_IsFetchingMessages = true;
                std::string ch = g_SelectedChannelId;
                std::thread([ch, firstId]() {
                    auto old_msgs = gc.FetchMessages(ch, firstId);
                    if (!old_msgs.empty()) {
                        std::lock_guard<std::mutex> lock(g_ChatMutex);
                        if(g_SelectedChannelId == ch) {
                            g_Messages.insert(g_Messages.begin(), old_msgs.begin(), old_msgs.end());
                            g_RestoreMsgId = firstId; // Anchor exactly to the old top message
                        }
                    }
                    g_IsFetchingMessages = false;
                }).detach();
            }
        }
        g_WasAtTop = isAtTop;
        {
            std::lock_guard<std::mutex> lock(g_ChatMutex);
            ImDrawList* dl=ImGui::GetWindowDrawList();
            const float AV_R=18.f,LEFT=16.f+36.f+10.f;
            float aw=ImGui::GetContentRegionAvail().x;
            std::string prevAid;
            for(auto&m:g_Messages){
                if (m.id == g_RestoreMsgId) {
                    ImGui::SetScrollHereY(0.0f); // 0.0 = top alignment
                    g_RestoreMsgId = "";
                }
                bool same=(m.author_id==prevAid);prevAid=m.author_id;
                if(!same){
                    ImGui::Dummy({0,8});
                    
                    // Render Reply Reference
                    if (!m.referenced_message_id.empty()) {
                        ImGui::SetCursorPosX(LEFT - 20);
                        ImDrawList* dl2 = ImGui::GetWindowDrawList();
                        ImVec2 cp2 = ImGui::GetCursorScreenPos();
                        dl2->AddLine({cp2.x + 5, cp2.y + 10}, {cp2.x + 15, cp2.y + 10}, IM_COL32(88, 101, 242, 255), 2.0f);
                        dl2->AddLine({cp2.x + 5, cp2.y + 10}, {cp2.x + 5, cp2.y + 25}, IM_COL32(88, 101, 242, 255), 2.0f);
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));
                        ImGui::SetWindowFontScale(0.85f);
                        std::string refTxt = m.referenced_author + ": " + m.referenced_content;
                        if (refTxt.size() > 60) refTxt = refTxt.substr(0, 57) + "...";
                        ImGui::TextUnformatted(refTxt.c_str());
                        ImGui::SetWindowFontScale(1.0f);
                        ImGui::PopStyleColor();
                    }

                    ImVec2 avSp=ImGui::GetCursorScreenPos();
                    DrawAvatarCircle(dl,{avSp.x+16.f+AV_R,avSp.y+AV_R},AV_R,m.author_id,m.author_avatar,m.author);
                    ImGui::SetCursorPosX(LEFT);
                    ImGui::PushStyleColor(ImGuiCol_Text,{0.93f,0.94f,0.96f,1.f});ImGui::TextUnformatted(m.author.c_str());ImGui::PopStyleColor();
                    if(!m.timestamp.empty()&&m.timestamp.size()>=16){
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,{0.38f,0.40f,0.44f,1.f});
                        ImGui::Text(" %s",m.timestamp.substr(11,5).c_str());ImGui::PopStyleColor();
                    }
                } else {ImGui::Dummy({0,1});}
                if(!m.content.empty()){
                    ImGui::SetCursorPosX(LEFT);
                    ImGui::PushTextWrapPos(aw-8.f);
                    ImGui::PushStyleColor(ImGuiCol_Text,{0.82f,0.83f,0.86f,1.f});
                    ImGui::TextUnformatted(m.content.c_str());
                    ImGui::PopStyleColor();ImGui::PopTextWrapPos();
                }
                for(auto&url:m.attachment_urls){
                    bool need=false;HTTPTexture tx={};
                    {std::lock_guard<std::mutex> tl(g_TextureMutex);if(!g_Textures.count(url)){g_Textures[url]={};need=true;}else tx=g_Textures[url];}
                    if(need)RequestTexture(url);
                    ImGui::SetCursorPosX(LEFT);
                    if(tx.view){
                        float dw=(float)tx.width,dh=(float)tx.height,maxW=aw-LEFT-8.f;
                        if(dw>maxW){dh*=maxW/dw;dw=maxW;}if(dh>300.f){dw*=300.f/dh;dh=300.f;}
                        ID3D11ShaderResourceView* dv=tx.view;
                        if(!tx.frames.empty()&&tx.total_duration>0){
                            int t=(int)(ImGui::GetTime()*1000.)%tx.total_duration,acc=0;
                            for(size_t f=0;f<tx.frames.size();++f){acc+=tx.delays[f];if(t<acc){dv=tx.frames[f];break;}}
                        }
                        ImVec2 cp=ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton(("##img"+m.id+url).c_str(),{dw,dh});
                        if(ImGui::IsItemHovered())ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if(ImGui::IsItemClicked())g_LightboxUrl=url;
                        ImGui::GetWindowDrawList()->AddImageRounded((void*)dv,cp,{cp.x+dw,cp.y+dh},{0,0},{1,1},IM_COL32_WHITE,4.f);
                    } else {ImGui::TextDisabled("[Loading...]");}
                }
                for(auto&url:m.video_urls){
                    bool need=false;VideoTexture vt={};
                    {std::lock_guard<std::mutex> vl(g_VideoMutex);if(!g_Videos.count(url)){g_Videos[url]={};need=true;}else vt=g_Videos[url];}
                    if(need)RequestVideo(url);
                    ImGui::SetCursorPosX(LEFT);
                    if(vt.view){ImGui::ImageButton(("VID"+url).c_str(),(void*)vt.view,{(float)vt.width*.4f,(float)vt.height*.4f});ImGui::TextDisabled("Video");}
                    else ImGui::TextDisabled("[Loading video...]");
                }
                if(!m.reactions.empty()){
                    ImGui::SetCursorPosX(LEFT);
                    for(auto&r:m.reactions){
                        ImGui::PushStyleColor(ImGuiCol_Button,r.me?ImVec4(0.35f,0.45f,0.9f,0.55f):ImVec4(0.18f,0.19f,0.22f,0.75f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,12.f);
                        if(ImGui::Button((r.emoji+" "+std::to_string(r.count)+"##r"+m.id).c_str())){
                            std::string cid=g_SelectedChannelId,mid=m.id,emo=r.emoji;
                            std::thread([cid,mid,emo](){gc.AddReaction(cid,mid,emo);RefreshMessages(cid);}).detach();
                        }
                        ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::SameLine(0,4);
                    }
                    ImGui::NewLine();
                }
                if(!m.id.empty()){
                    if(ImGui::BeginPopupContextItem(("CTX_"+m.id).c_str())){
                        if(ImGui::MenuItem("Reply")){
                            g_ReplyToId = m.id;
                            g_ReplyToAuthor = m.author;
                            g_ReplyToContent = m.content;
                            g_RefocusInput = true;
                        }
                        if(ImGui::BeginMenu("React")){
                            const char* quick_emojis[] = {"👍", "❤️", "😂", "😮", "😢", "🔥"};
                            for(auto em : quick_emojis) {
                                if(ImGui::MenuItem(em)) {
                                    std::string cid=g_SelectedChannelId,mid=m.id,emo=em;
                                    std::thread([cid,mid,emo](){gc.AddReaction(cid,mid,emo);RefreshMessages(cid);}).detach();
                                }
                            }
                            ImGui::EndMenu();
                        }
                        if(m.author_id==gc.GetUserId()){
                            if(ImGui::MenuItem("Edit")){g_EditingMessageId=m.id;strncpy_s(g_EditBuffer,m.content.c_str(),_TRUNCATE);}
                            if(ImGui::MenuItem("Delete")){std::string cid=g_SelectedChannelId,mid=m.id;std::thread([cid,mid](){gc.DeleteMessage(cid,mid);RefreshMessages(cid);}).detach();}
                        }
                        ImGui::EndPopup();
                    }
                }
                if(g_EditingMessageId==m.id){
                    ImGui::SetCursorPosX(LEFT);
                    ImGui::PushItemWidth(aw-LEFT-70.f);
                    ImGui::InputText(("##ed"+m.id).c_str(),g_EditBuffer,sizeof(g_EditBuffer));
                    ImGui::PopItemWidth();ImGui::SameLine();
                    if(ImGui::Button("Save",{58,0})){std::string cid=g_SelectedChannelId,mid=m.id,msg=g_EditBuffer;std::thread([cid,mid,msg](){gc.EditMessage(cid,mid,msg);RefreshMessages(cid);}).detach();g_EditingMessageId="";}
                }
            }
            if(ImGui::GetScrollY()>=ImGui::GetScrollMaxY())ImGui::SetScrollHereY(1.f);
        }
        ImGui::EndChild();
        // Input bar — wrapped with padding
        ImGui::Dummy({0,4});
        ImGui::SetCursorPosX(8.f);
        
        // ── REPLY STAGING AREA ──────────────────────────────────────
        if (!g_ReplyToId.empty()) {
            ImGui::SetCursorPosX(8.f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f,0.19f,0.22f,1.f));
            ImGui::BeginChild("ReplyStaging", {ImGui::GetContentRegionAvail().x - 8, 30}, false);
            ImGui::SetCursorPos({10, 6});
            ImGui::TextDisabled("Replying to"); ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.9f, 1.0f), "%s", g_ReplyToAuthor.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 30);
            if (ImGui::Button("X", {20, 20})) { g_ReplyToId = ""; g_ReplyToAuthor = ""; g_ReplyToContent = ""; }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── COMPOSE STAGING AREA ─────────────────────────────────────
        if (g_ComposeOpen && !g_ComposeFiles.empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f,0.17f,0.19f,1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
            ImGui::BeginChild("ComposeStaging", {0, 60}, true, ImGuiWindowFlags_NoScrollbar);
            for(size_t i=0; i<g_ComposeFiles.size(); ++i){
                ImGui::PushID((int)i);
                ImGui::BeginChild("FileCard", {200, 44}, true, ImGuiWindowFlags_NoScrollbar);
                // Extract filename
                std::string path = g_ComposeFiles[i];
                std::string filename = path.substr(path.find_last_of("/\\") + 1);
                ImGui::SetCursorPos({10, 14});
                ImGui::TextUnformatted(filename.c_str());
                ImGui::SameLine(160);
                ImGui::SetCursorPosY(10);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f,0.2f,0.2f,0.6f));
                if(ImGui::Button("X", {24,24})) {
                    g_ComposeFiles.erase(g_ComposeFiles.begin() + i);
                    if(g_ComposeFiles.empty()) g_ComposeOpen = false;
                }
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopID();
                if(i < g_ComposeFiles.size() - 1) ImGui::SameLine();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorPosX(8.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0.16f,0.17f,0.19f,1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,8.f);
        if(ImGui::Button("+",{28,28})){
            std::string p=OpenMediaFileDialog();
            if(!p.empty()){
                g_ComposeFiles.push_back(p);
                g_ComposeOpen = true;
            }
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x-74.f);
        if (g_RefocusInput) { ImGui::SetKeyboardFocusHere(0); g_RefocusInput = false; }
        bool doSend=ImGui::InputText("##inp",g_InputBuffer,sizeof(g_InputBuffer),ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();ImGui::SameLine();
        doSend|=ImGui::Button("Send",{55,28});
        
        if(doSend && !g_SelectedChannelId.empty()){
            bool hasText = strlen(g_InputBuffer) > 0;
            bool hasFiles = !g_ComposeFiles.empty();
            if(hasText || hasFiles){
                std::string ct=g_InputBuffer,ch=g_SelectedChannelId;
                std::vector<std::string> files = g_ComposeFiles;
                memset(g_InputBuffer,0,sizeof(g_InputBuffer));
                g_ComposeFiles.clear();
                g_ComposeOpen = false;
                std::thread([ch, ct, files, hasText, hasFiles, replyId = g_ReplyToId](){
                    for(const auto& f : files) gc.SendAttachment(ch, f);
                    if(hasText) {
                        if(!replyId.empty()) gc.SendReply(ch, ct, replyId);
                        else gc.SendDiscordMessage(ch, ct);
                    }
                    RefreshMessages(ch);
                }).detach();
                g_ReplyToId = ""; g_ReplyToAuthor = ""; g_ReplyToContent = "";
            }
            g_RefocusInput = true; // restore focus next frame
        }
        ImGui::PopStyleVar();ImGui::PopStyleColor();
    }
    // ── LIGHTBOX OVERLAY ─────────────────────────────────────────
    if(!g_LightboxUrl.empty()){
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0,0,0,0.85f));
        if(ImGui::Begin("Lightbox",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoBringToFrontOnFocus)){
            ImDrawList* dl=ImGui::GetWindowDrawList();
            HTTPTexture tx={};{std::lock_guard<std::mutex> tl(g_TextureMutex);if(g_Textures.count(g_LightboxUrl))tx=g_Textures[g_LightboxUrl];}
            if(tx.view){
                float dw=(float)tx.width, dh=(float)tx.height;
                float vpW=ImGui::GetWindowWidth(), vpH=ImGui::GetWindowHeight();
                float scale=(std::min)(vpW/dw, vpH/dh);
                if(scale>1.f)scale=1.f;
                dw*=scale; dh*=scale;
                ImVec2 p0={ImGui::GetWindowPos().x+(vpW-dw)*0.5f,ImGui::GetWindowPos().y+(vpH-dh)*0.5f};
                
                ID3D11ShaderResourceView* dv=tx.view;
                if(!tx.frames.empty()&&tx.total_duration>0){
                    int t=(int)(ImGui::GetTime()*1000.)%tx.total_duration,acc=0;
                    for(size_t f=0;f<tx.frames.size();++f){acc+=tx.delays[f];if(t<acc){dv=tx.frames[f];break;}}
                }
                dl->AddImage((void*)dv,p0,{p0.x+dw,p0.y+dh});
            }
            if(ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) g_LightboxUrl="";
            if(ImGui::IsKeyPressed(ImGuiKey_Escape)) g_LightboxUrl="";
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();
    ImGui::End();
    
    DrawIncomingCallOverlay();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int RunGUI() {
    MFStartup(MF_VERSION);
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("TokenTalks"), nullptr };
    ::RegisterClassEx(&wc);
    HWND hwnd = ::CreateWindow(wc.lpszClassName, _T("Token-Talks"), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)(GetSystemMetrics(SM_CXSCREEN) * 0.75f),
        (int)(GetSystemMetrics(SM_CYSCREEN) * 0.8f),
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    LoadSettings();
    if (strlen(g_Settings.font_path) > 0) {
        if (!io.Fonts->AddFontFromFileTTF(g_Settings.font_path, 18.0f)) {
            io.Fonts->AddFontDefault();
        }
    } else {
        io.Fonts->AddFontDefault();
    }
    ApplyTheme(g_Settings.theme);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    gc.SetOnMessageCallback(OnDiscordMessage);
    gc.SetOnCallCallback(OnIncomingCall);

    LoadAccountsGUI();

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!g_IsLoggedIn) { DrawDashboard(); }
        else { DrawMainApp(); }

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);

    gc.Disconnect();
    ExitProcess(0);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}