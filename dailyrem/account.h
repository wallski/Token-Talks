#pragma once
#include <string>
#include <vector>

enum class AccountType { TOKEN, EMAIL };

struct Account {
    std::string name;
    std::string token;
    AccountType type = AccountType::TOKEN;
};

void LoadTokenAccounts(std::vector<Account>& list);
void SaveTokenAccounts(const std::vector<Account>& list);

void LoadMailAccounts(std::vector<Account>& list);
void SaveMailAccounts(const std::vector<Account>& list);