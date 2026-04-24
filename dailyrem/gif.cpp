#define NOMINMAX
#include "gif.h"
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




bool FindGifByName(const std::string& name, std::string& url) {
    std::vector<GifEntry> list;
    LoadGifs(list);
    for (const auto& g : list)
        if (g.name == name) { url = g.url; return true; }
    return false;
}