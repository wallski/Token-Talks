#include "commands.h"
#include "gif.h"
#include <iostream>
#include <cstdlib>

static std::unordered_map<std::string, CommandFunc> g_cmds;
static CommandFunc g_sendFn;

static CmdResult CmdHelp(const std::string&) {
    std::cout << "Available commands:\n"
        << "  /help        - this list\n"
        << "  /clear       - clear screen\n"
        << "  /exit        - leave chat loop\n"
        << "  /spam N txt  - send txt N times\n"
        << "  /gif name    - send GIF\n"
        << "  /gifs        - show saved GIFs\n";
    return CmdResult::Ok;
}

static CmdResult CmdClear(const std::string&) {
#ifdef _WIN32
    system("cls");
#endif
    return CmdResult::Ok;
}

static CmdResult CmdExit(const std::string&) {
    std::cout << "Leaving chat\n";
    return CmdResult::RequestQuitLoop;
}

static CmdResult CmdSpam(const std::string& args) {
    size_t space1 = args.find(' ');
    if (space1 == std::string::npos) {
        std::cout << "Usage: /spam <count> <message>\n";
        return CmdResult::Ok;
    }
    int count = std::stoi(args.substr(0, space1));
    std::string text = args.substr(space1 + 1);
    for (int i = 0; i < count; ++i) {
        if (g_sendFn) g_sendFn(text);
    }
    return CmdResult::Ok;
}

static CmdResult CmdGif(const std::string& args) {
    if (args.empty()) {
        std::cout << "Usage: /gif <name>\n";
        return CmdResult::Ok;
    }
    std::string url;
    if (!FindGifByName(args, url)) {
        std::cout << "No GIF named '" << args << "' found.\n";
        return CmdResult::Ok;
    }
    if (g_sendFn) g_sendFn(url);
    return CmdResult::Ok;
}

static CmdResult CmdGifsList(const std::string&) {
    std::vector<GifEntry> g;
    LoadGifs(g);
    if (g.empty()) {
        std::cout << "No GIFs saved.\n";
        return CmdResult::Ok;
    }
    std::cout << "GIF list:\n";
    for (const auto& i : g) std::cout << "  " << i.name << '\n';
    return CmdResult::Ok;
}

void RegisterCommands() {
    g_cmds["gif"] = CmdGif;
    g_cmds["help"] = CmdHelp;
    g_cmds["clear"] = CmdClear;
    g_cmds["exit"] = CmdExit;
    g_cmds["spam"] = CmdSpam;
    g_cmds["gifs"] = CmdGifsList;
}

bool IsCommand(const std::string& msg) {
    return !msg.empty() && msg.front() == '/';
}

CmdResult ExecuteCommand(const std::string& msg) {
    std::string inner = msg.substr(1);
    size_t space = inner.find(' ');
    std::string name = (space == std::string::npos) ? inner : inner.substr(0, space);
    std::string args = (space == std::string::npos) ? "" : inner.substr(space + 1);

    auto it = g_cmds.find(name);
    if (it == g_cmds.end()) {
        std::cout << "Unknown command: /" << name << "\n";
        return CmdResult::Ok;
    }
    return it->second(args);
}

void SetSendFunction(const CommandFunc& f) {
    g_sendFn = f;
}