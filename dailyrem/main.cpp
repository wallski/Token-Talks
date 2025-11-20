#define NOMINMAX
#include "account.h"
#include "functions.h"
#include "commands.h"
#include "gif.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <string>
#include <limits>
#include <atomic>
#include <thread>

#pragma comment(lib, "winhttp.lib")

int main() {
    RegisterCommands();

    int menuOption;
    while (true) {
        clearScreen();
        std::cout << "====== token talks ======\n"
            << "1) account manager\n"
            << "2) gif manager\n"
            << "3) token talk\n"
            << "4) exit\n\nSelect an option: ";
        std::cin >> menuOption;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        if (menuOption == 1) {
            AccountManagerLoop();
        }
        else if (menuOption == 2) {
            GifManagerLoop();
        }
        else if (menuOption == 3) {
            Account acc;
            if (!PickAccount(acc))
                continue;

            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScreen();
            std::string channelURL;
            std::cout << "Enter channel URL: ";
            std::getline(std::cin, channelURL);

            size_t lastSlash = channelURL.find_last_of('/');
            if (lastSlash == std::string::npos) {
                std::cout << "Invalid URL format!\n";
                pauseAndClear();
                continue;
            }

            std::string channelID = channelURL.substr(lastSlash + 1);
            std::string apiPath = "/api/channels/" + channelID + "/messages";
            std::wstring wApiPath(apiPath.begin(), apiPath.end());

            HINTERNET session = WinHttpOpen(L"msgEZ/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            HINTERNET connect = WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);

            std::string authHeader = "Authorization: " + acc.token + "\r\n";
            std::string contentHeader = "Content-Type: application/json\r\n";
            std::string combinedHeaders = authHeader + contentHeader;
            std::wstring wheaders(combinedHeaders.begin(), combinedHeaders.end());

            auto SendToDiscord = [&](const std::string& text) -> CmdResult {
                std::string escaped = text;
                size_t pos = 0;
                while ((pos = escaped.find('"', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\\"");
                    pos += 2;
                }
                std::string payload = "{\"content\":\"" + escaped + "\"}";

                HINTERNET request = WinHttpOpenRequest(connect, L"POST",
                    wApiPath.c_str(), NULL, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

                BOOL sent = WinHttpSendRequest(request,
                    wheaders.c_str(), -1,
                    (LPVOID)payload.c_str(), (DWORD)payload.size(),
                    (DWORD)payload.size(), 0);

                if (!sent) {
                    std::cout << "Failed to send message!\n";
                    WinHttpCloseHandle(request);
                    return CmdResult::Ok;
                }

                WinHttpReceiveResponse(request, NULL);
                DWORD statusCode = 0;
                DWORD size = sizeof(statusCode);
                WinHttpQueryHeaders(request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &statusCode, &size, NULL);

                WinHttpCloseHandle(request);
                if (statusCode == 200 || statusCode == 204) {
                    std::cout << "Message sent!\n";
                    return CmdResult::Ok;
                }
                else {
                    std::cout << "Server returned status: " << statusCode << "\n";
                    return CmdResult::Ok;
                }
                };

            SetSendFunction(SendToDiscord);

            std::cout << "Connected! Type messages below (type /help to list commands):\n";
            std::string message;
            while (true) {
                std::getline(std::cin, message);
                if (message == "exit") break;

                if (IsCommand(message)) {
                    if (ExecuteCommand(message) == CmdResult::RequestQuitLoop)
                        break;
                    continue;
                }

                SendToDiscord(message);
            }

            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            pauseAndClear();
        }
        else if (menuOption == 4) {
            clearScreen();
            std::cout << "Goodbye!\n";
            break;
        }
        else {
            std::cout << "Invalid option!\n";
            pauseAndClear();
        }
    }
    return 0;
}