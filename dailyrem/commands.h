#pragma once
#include <string>
#include <functional>
#include <unordered_map>

enum class CmdResult { Ok, RequestQuitLoop };

using CommandFunc = std::function<CmdResult(const std::string& args)>;

void RegisterCommands();
bool IsCommand(const std::string& msg);
CmdResult ExecuteCommand(const std::string& msg);
void SetSendFunction(const CommandFunc& f);