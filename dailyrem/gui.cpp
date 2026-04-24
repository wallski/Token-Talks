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
};
static AppSettings g_Settings;
static bool g_ShowSettings = false;
static std::string g_EditingMessageId = "";
static char g_EditBuffer[1024] = "";

void LoadSettings() {
    std::ifstream file("settings.json");
    if (file) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        try {
            auto j = nlohmann::json::parse(content);
            if (j.contains("theme")) g_Settings.theme = j["theme"].get<int>();
            if (j.contains("show_private_channels")) g_Settings.show_private_channels = j["show_private_channels"].get<bool>();
        } catch(...) {}
    }
}

void SaveSettings() {
    nlohmann::json j;
    j["theme"] = g_Settings.theme;
    j["show_private_channels"] = g_Settings.show_private_channels;
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
}

struct HTTPTexture {
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0;
    int height = 0;
};
static std::map<std::string, HTTPTexture> g_Textures;
static std::mutex g_TextureMutex;

void RequestTexture(const std::string& url) {
    if (url.empty()) return;
    std::thread([url]() {
        auto bytes = gc.DownloadFile(url);
        if (bytes.empty()) return;
        int w, h, n;
        unsigned char* data = stbi_load_from_memory(bytes.data(), bytes.size(), &w, &h, &n, 4);
        if (!data) return;
        
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
        subResource.pSysMem = data;
        subResource.SysMemPitch = desc.Width * 4;
        subResource.SysMemSlicePitch = 0;

        ID3D11Texture2D* pTexture = nullptr;
        ID3D11ShaderResourceView* pSrv = nullptr;

        if (g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture) == S_OK) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
            ZeroMemory(&srvDesc, sizeof(srvDesc));
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = desc.MipLevels;
            g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSrv);
            pTexture->Release();
        }
        stbi_image_free(data);

        if (pSrv) {
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            g_Textures[url] = { pSrv, w, h };
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
    g_SelectedChannelId = channelId;
    {
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

void DrawDashboard() {
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImVec2 vp_pos = ImGui::GetMainViewport()->Pos;
    ImGui::SetNextWindowPos(vp_pos);
    ImGui::SetNextWindowSize(vp);
    ImGui::Begin("Dashboard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    float panelW = (std::max)(280.0f, (std::min)(vp.x * 0.38f, 440.0f));

    float panelX = (vp.x - panelW) * 0.5f;
    float startY = vp.y * 0.12f;

    ImGui::SetCursorPos(ImVec2(panelX, startY));
    ImGui::BeginGroup();

    ImVec2 subSize = ImGui::CalcTextSize("welcome to");
    ImGui::SetCursorPosX(panelX + panelW * 0.5f - subSize.x * 0.5f);
    ImGui::TextDisabled("welcome to");

    ImGui::SetWindowFontScale(2.2f);
    ImVec2 mainTitleSize = ImGui::CalcTextSize("Token-Talks");
    ImGui::SetCursorPosX(panelX + panelW * 0.5f - mainTitleSize.x * 0.5f);
    ImGui::TextColored(ImVec4(0.35f, 0.40f, 0.94f, 1.0f), "Token-Talks");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Dummy(ImVec2(0, 14));

    ImGui::SetCursorPosX(panelX);
    ImGui::BeginChild("AccountList", ImVec2(panelW, 170), true);
    auto drawAccountRow = [&](std::vector<Account>& list, bool ismail) {
        for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID((ismail ? 10000 : 0) + (int)i);
            std::string label = (ismail ? "[M] " : "[T] ") + list[i].name;
            if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 38, 32))) {
                gc.SetToken(list[i].token);
                gc.SetOnMessageCallback(OnDiscordMessage);
                gc.Connect();
                g_IsLoggedIn = true;
                std::thread([]() { RefreshGuilds(); }).detach();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("X", ImVec2(32, 32))) {
                list.erase(list.begin() + i);
                if (ismail) SaveMailAccounts(list); else SaveTokenAccounts(list);
                ImGui::PopStyleColor(2); ImGui::PopID(); break;
            }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    };
    drawAccountRow(g_TokenAccounts, false);
    drawAccountRow(g_MailAccounts, true);
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SetCursorPosX(panelX);

    ImGui::PushItemWidth(panelW);
    if (ImGui::BeginTabBar("##AddTabs")) {

        if (ImGui::BeginTabItem("Token Login")) {
            static char newName[128] = "";
            static char newToken[256] = "";
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::InputTextWithHint("##TName", "Account Name", newName, sizeof(newName));
            ImGui::InputTextWithHint("##TToken", "Discord Token", newToken, sizeof(newToken), ImGuiInputTextFlags_Password);
            if (ImGui::Button("Add Token Account", ImVec2(panelW, 32))) {
                if (strlen(newName) > 0 && strlen(newToken) > 0) {
                    if (DiscordClient::ValidateToken(newToken)) {
                        Account acc = { newName, newToken, AccountType::TOKEN };
                        g_TokenAccounts.push_back(acc);
                        SaveTokenAccounts(g_TokenAccounts);
                        memset(newName, 0, sizeof(newName));
                        memset(newToken, 0, sizeof(newToken));
                        g_TokenError = "";
                    } else { g_TokenError = "Invalid token — could not verify with Discord."; }
                }
            }
            if (!g_TokenError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", g_TokenError.c_str());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Email Login")) {
            static char newName[128] = "";
            static char newEmail[256] = "";
            static char newPass[256] = "";
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.5f, 0.25f, 0.0f, 0.3f));
            ImGui::BeginChild("TosWarn", ImVec2(panelW, 34), false);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "  ! Violates Discord ToS — use at your own risk.");
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::InputTextWithHint("##MName", "Display Name", newName, sizeof(newName));
            ImGui::InputTextWithHint("##MEmail", "Email", newEmail, sizeof(newEmail));
            ImGui::InputTextWithHint("##MPass", "Password", newPass, sizeof(newPass), ImGuiInputTextFlags_Password);
            if (ImGui::Button("Login with Email", ImVec2(panelW, 32))) {
                if (strlen(newName) > 0 && strlen(newEmail) > 0 && strlen(newPass) > 0) {
                    g_MailError = "Authenticating...";
                    std::string name = newName;
                    std::string email = newEmail;
                    std::string pass = newPass;
                    strncpy_s(g_PendingMailName, sizeof(g_PendingMailName), name.c_str(), _TRUNCATE);
                    std::thread([name, email, pass]() {
                        std::string mfa_ticket;
                        std::string token = DiscordClient::LoginWithCredentials(email, pass, mfa_ticket);
                        if (!token.empty()) {
                            Account acc = { name, token, AccountType::EMAIL };
                            g_MailAccounts.push_back(acc);
                            SaveMailAccounts(g_MailAccounts);
                            g_MailError = "Account added!";
                        } else if (!mfa_ticket.empty()) {
                            g_MfaTicket = mfa_ticket;
                            g_ShowMfaModal = true;
                            g_MailError = "";
                        } else {
                            g_MailError = "Login failed. Check credentials.";
                        }
                    }).detach();
                    memset(newName, 0, sizeof(newName));
                    memset(newEmail, 0, sizeof(newEmail));
                    memset(newPass, 0, sizeof(newPass));
                }
            }
            if (!g_MailError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", g_MailError.c_str());
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();

    ImGui::EndGroup();

    if (g_ShowMfaModal) {
        ImGui::OpenPopup("MFA Required");
    }
    ImVec2 center = ImVec2(vp_pos.x + vp.x * 0.5f, vp_pos.y + vp.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("MFA Required", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Two-Factor Authentication required.");
        ImGui::Text("Enter your 6-digit authenticator code:");
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushItemWidth(200);
        ImGui::InputTextWithHint("##MfaCode", "000000", g_MfaCode, sizeof(g_MfaCode));
        ImGui::PopItemWidth();
        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("Confirm", ImVec2(95, 0))) {
            std::string ticket = g_MfaTicket;
            std::string code = g_MfaCode;
            std::string name = g_PendingMailName;
            std::thread([ticket, code, name]() {
                std::string token = DiscordClient::SubmitMfaCode(code, ticket);
                if (!token.empty()) {
                    Account acc = { name, token, AccountType::EMAIL };
                    g_MailAccounts.push_back(acc);
                    SaveMailAccounts(g_MailAccounts);
                    g_MailError = "Account added!";
                } else {
                    g_MailError = "Invalid MFA code.";
                }
            }).detach();
            g_ShowMfaModal = false;
            g_MfaTicket = "";
            memset(g_MfaCode, 0, sizeof(g_MfaCode));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(95, 0))) {
            g_ShowMfaModal = false;
            g_MfaTicket = "";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void DrawMainApp() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
    ImGui::Begin("MainApp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::Text("Logged in as User ID: %s", gc.GetUserId().c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    if (ImGui::Button("Settings", ImVec2(90, 0))) {
        g_ShowSettings = !g_ShowSettings;
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    if (ImGui::Button("Logout", ImVec2(80, 0))) {
        std::thread([]() { gc.Disconnect(); }).detach();
        g_IsLoggedIn = false;
        g_ShowSettings = false; // Reset on logout
    }
    ImGui::Separator();
    
    if (g_ShowSettings) {
        ImGui::BeginChild("SettingsPane", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::Text("Client Settings");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Text("Interface Theme");
        const char* themes[] = { "Modern Blurple", "Midnight Stealth", "Ruby Crimson", "Classic Light", "Amethyst Dark" };
        ImGui::PushItemWidth(300);
        if (ImGui::Combo("##ThemeSelect", &g_Settings.theme, themes, IM_ARRAYSIZE(themes))) {
            ApplyTheme(g_Settings.theme);
            SaveSettings();
        }
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Text("Network Sync Options");
        if (ImGui::Checkbox("Show Private / Locked Channels", &g_Settings.show_private_channels)) {
            SaveSettings();
        }

        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // Left Panel (Servers)
    ImGui::BeginChild("Servers", ImVec2(200, 0), true);
    ImGui::Text("Servers");
    ImGui::Separator();
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);
        for (const auto& g : g_Guilds) {
            bool selected = (g.id == g_SelectedGuildId);
            if (ImGui::Selectable(g.name.c_str(), selected)) {
                std::thread([g]() { RefreshChannels(g.id); }).detach();
            }
        }
    }
    ImGui::EndChild();
    
    ImGui::SameLine();

    // Middle Panel (Channels)
    ImGui::BeginChild("Channels", ImVec2(200, 0), true);
    ImGui::Text("Channels");
    ImGui::Separator();
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);
        for (const auto& c : g_Channels) {
            if (!g_Settings.show_private_channels && c.is_locked) continue;
            
            bool selected = (c.id == g_SelectedChannelId);
            std::string displayName = c.name;
            if (c.is_locked) displayName = "[?] " + displayName;

            if (ImGui::Selectable(displayName.c_str(), selected)) {
                std::thread([c]() { RefreshMessages(c.id); }).detach();
            }
        }
    }
    ImGui::EndChild();
    
    ImGui::SameLine();

    // Right Panel (Chat)
    ImGui::BeginChild("Chat", ImVec2(0, 0), true);
    ImGui::Text("Chat: %s", g_SelectedChannelId.c_str());
    ImGui::Separator();

    ImGui::BeginChild("MessagesScroll", ImVec2(0, -45), true);
    {
        std::lock_guard<std::mutex> lock(g_ChatMutex);
        for (const auto& m : g_Messages) {
            float avail_w = ImGui::GetContentRegionAvail().x;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.22f, 0.23f, 0.27f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

            float img_total_h = 0.0f;
            std::vector<std::pair<float,float>> img_sizes;
            for (const auto& url : m.attachment_urls) {
                std::lock_guard<std::mutex> tl(g_TextureMutex);
                if (g_Textures.count(url) && g_Textures[url].view) {
                    float drawW = (float)g_Textures[url].width;
                    float drawH = (float)g_Textures[url].height;
                    float maxW = avail_w - 20.0f;
                    if (drawW > maxW) { drawH = drawH * (maxW / drawW); drawW = maxW; }
                    if (drawH > 300.0f) { drawW = drawW * (300.0f / drawH); drawH = 300.0f; }
                    img_sizes.push_back({drawW, drawH});
                    img_total_h += drawH + 8.0f;
                } else {
                    img_sizes.push_back({0, 20.0f});
                    img_total_h += 28.0f;
                }
            }

            float text_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + img_total_h + 16.0f;
            bool canAutoSize = text_h > 0;
            ImGui::BeginChild(("msg_" + m.id).c_str(), ImVec2(avail_w - 4, text_h), false,
                              ImGuiWindowFlags_NoScrollbar);

            ImGui::TextColored(ImVec4(0.55f, 0.65f, 1.0f, 1.0f), "%s", m.author.c_str());
            if (!m.content.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                ImGui::TextUnformatted(m.content.c_str());
                ImGui::PopTextWrapPos();
            }

            size_t idx = 0;
            for (const auto& url : m.attachment_urls) {
                bool needLoad = false;
                HTTPTexture localTex = {nullptr,0,0};
                {
                    std::lock_guard<std::mutex> tl(g_TextureMutex);
                    if (g_Textures.count(url) == 0) { g_Textures[url] = {}; needLoad = true; }
                    else localTex = g_Textures[url];
                }
                if (needLoad) RequestTexture(url);

                if (localTex.view && idx < img_sizes.size()) {
                    ImGui::Image((void*)localTex.view, ImVec2(img_sizes[idx].first, img_sizes[idx].second));
                } else {
                    ImGui::TextDisabled("[ Loading... ]");
                }
                idx++;
                ImGui::Dummy(ImVec2(0, 4));
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            
            if (m.author_id == gc.GetUserId() && !m.id.empty()) {
                if (ImGui::BeginPopupContextItem(("CTX_" + m.id).c_str())) {
                    if (ImGui::MenuItem("Edit Message")) {
                        g_EditingMessageId = m.id;
                        strncpy_s(g_EditBuffer, sizeof(g_EditBuffer), m.content.c_str(), _TRUNCATE);
                    }
                    if (ImGui::MenuItem("Delete Message")) {
                        std::string cid = g_SelectedChannelId;
                        std::string mid = m.id;
                        std::thread([cid, mid]() {
                            gc.DeleteMessage(cid, mid);
                            RefreshMessages(cid);
                        }).detach();
                    }
                    ImGui::EndPopup();
                }
            }

            if (g_EditingMessageId == m.id) {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 60);
                ImGui::InputText(("##Edit" + m.id).c_str(), g_EditBuffer, sizeof(g_EditBuffer));
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("Save", ImVec2(50, 0))) {
                    std::string cid = g_SelectedChannelId;
                    std::string mid = m.id;
                    std::string msg = g_EditBuffer;
                    std::thread([cid, mid, msg]() {
                        gc.EditMessage(cid, mid, msg);
                        RefreshMessages(cid);
                    }).detach();
                    g_EditingMessageId = "";
                }
            }

            ImGui::Dummy(ImVec2(0, 8));
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 100);

    if (ImGui::Button("+", ImVec2(30, 24))) {
        std::string path = OpenMediaFileDialog();
        if (!path.empty() && !g_SelectedChannelId.empty()) {
            std::string chId = g_SelectedChannelId;
            std::thread([chId, path]() {
                gc.SendAttachment(chId, path);
                RefreshMessages(chId);
            }).detach();
        }
    }
    ImGui::SameLine();

    if (ImGui::InputText("##Send", g_InputBuffer, sizeof(g_InputBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (strlen(g_InputBuffer) > 0 && !g_SelectedChannelId.empty()) {
            std::string content = g_InputBuffer;
            std::string chId = g_SelectedChannelId;
            memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
            std::thread([chId, content]() {
                gc.SendDiscordMessage(chId, content);
                RefreshMessages(chId);
            }).detach();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        if (strlen(g_InputBuffer) > 0 && !g_SelectedChannelId.empty()) {
            std::string content = g_InputBuffer;
            std::string chId = g_SelectedChannelId;
            memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
            std::thread([chId, content]() {
                gc.SendDiscordMessage(chId, content);
                RefreshMessages(chId);
            }).detach();
        }
    }
    ImGui::PopItemWidth();
    
    ImGui::EndChild();
    
    ImGui::End();
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
    ApplyTheme(g_Settings.theme);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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