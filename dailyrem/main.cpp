#include <windows.h>
#include <string>
#include <filesystem>
#include <fstream>
#include "gui.h"

bool ExtractResource(int resourceId, const std::filesystem::path& destPath) {
    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return false;

    DWORD size = SizeofResource(hModule, hRes);
    void* ptr = LockResource(hData);

    std::ofstream ofs(destPath, std::ios::binary);
    if (!ofs) return false;
    ofs.write((char*)ptr, size);
    return true;
}

int main() {
    // Extract DLLs to a temp directory for a single-file experience
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "TokenTalks_Runtime";
    std::filesystem::create_directories(tempDir);

    struct ResourceInfo { int id; std::string name; };
    ResourceInfo dlls[] = {
        {101, "dpp.dll"}, {102, "libcrypto-1_1-x64.dll"}, {103, "libdave.dll"},
        {104, "libsodium.dll"}, {105, "libssl-1_1-x64.dll"}, {106, "opus.dll"},
        {107, "zlib1.dll"}
    };

    for (const auto& dll : dlls) {
        ExtractResource(dll.id, tempDir / dll.name);
    }

    // Point the DLL loader to our extracted files
    SetDllDirectoryA(tempDir.string().c_str());

    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_HIDE);
    }
    
    return RunGUI();
}