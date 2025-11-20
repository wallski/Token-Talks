#pragma once
#include <string>
#include <vector>

struct Account {
    std::string name;
    std::string token;
};

// disk IO
void LoadAccounts(std::vector<Account>& list);
void SaveAccounts(const std::vector<Account>& list);

// menus
void AccountManagerLoop();
bool PickAccount(Account& out);