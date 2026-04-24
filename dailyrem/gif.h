#pragma once
#include <string>
#include <vector>

struct GifEntry {
    std::string name;
    std::string url;
};

void LoadGifs(std::vector<GifEntry>& list);
void SaveGifs(const std::vector<GifEntry>& list);
bool UrlLooksAlive(const std::string& url);
bool FindGifByName(const std::string& name, std::string& url);