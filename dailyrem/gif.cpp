#define NOMINMAX
#include "gif.h"
#include "functions.h"
#include <fstream>
#include <iostream>
#include <windows.h>
#include <winhttp.h>
#include <wininet.h>
#include <limits>

#pragma comment(lib, "wininet.lib")

static const char* GIF_FILE = "gifs.txt";

void LoadGifs(std::vector<GifEntry>& list) {
    list.clear();
    std::ifstream f(GIF_FILE);
    if (!f) return;
    GifEntry g;
    while (std::getline(f, g.name) && std::getline(f, g.url))
        list.push_back(g);
}

void SaveGifs(const std::vector<GifEntry>& list) {
    std::ofstream f(GIF_FILE);
    for (const auto& g : list)
        f << g.name << '\n' << g.url << '\n';
}


bool UrlLooksAlive(const std::string&) { return true; }


static void AddGif(std::vector<GifEntry>& list) {
    clearScreen();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    GifEntry g;
    std::cout << "GIF name: ";
    std::getline(std::cin, g.name);
    std::cout << "GIF URL: ";
    std::getline(std::cin, g.url);

    if (!UrlLooksAlive(g.url)) {
        std::cout << "URL does not look alive, not saving.\n";
        pauseAndClear();
        return;
    }
    list.push_back(g);
    SaveGifs(list);
    std::cout << "GIF added.\n";
    pauseAndClear();
}

static void RemoveGif(std::vector<GifEntry>& list) {
    clearScreen();
    if (list.empty()) {
        std::cout << "No GIFs to remove.\n";
        pauseAndClear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i)
        std::cout << i + 1 << ") " << list[i].name << '\n';
    std::cout << list.size() + 1 << ") back\n\nchoice: ";
    int c;  std::cin >> c;
    if (c >= 1 && c <= static_cast<int>(list.size())) {
        list.erase(list.begin() + (c - 1));
        SaveGifs(list);
        std::cout << "Removed.\n";
    }
    pauseAndClear();
}


void GifManagerLoop() {
    std::vector<GifEntry> list;
    LoadGifs(list);
    while (true) {
        clearScreen();
        std::cout << "----- gif manager -----\n"
            << "1) add gif\n"
            << "2) remove gif\n"
            << "3) back\n\nchoice: ";
        int choice;
        std::cin >> choice;
        if (choice == 3) break;
        if (choice == 1) AddGif(list);
        if (choice == 2) RemoveGif(list);
    }
}

bool FindGifByName(const std::string& name, std::string& url) {
    std::vector<GifEntry> list;
    LoadGifs(list);
    for (const auto& g : list)
        if (g.name == name) { url = g.url; return true; }
    return false;
}