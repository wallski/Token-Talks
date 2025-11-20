#define NOMINMAX
#include "account.h"
#include "functions.h"
#include <fstream>
#include <iostream>
#include <limits>

static const char* FILE_NAME = "accounts.txt";

void LoadAccounts(std::vector<Account>& list) {
    list.clear();
    std::ifstream f(FILE_NAME);
    if (!f) return;
    Account a;
    while (std::getline(f, a.name) && std::getline(f, a.token))
        list.push_back(a);
}

void SaveAccounts(const std::vector<Account>& list) {
    std::ofstream f(FILE_NAME);
    for (const auto& a : list)
        f << a.name << '\n' << a.token << '\n';
}

static void AddTokenAccount(std::vector<Account>& list) {
    clearScreen();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    Account a;
    std::cout << "Give this account a name: ";
    std::getline(std::cin, a.name);
    std::cout << "Paste the token (Bot or user): ";
    std::getline(std::cin, a.token);
    list.push_back(a);
    SaveAccounts(list);
    std::cout << "Account added.\n";
    pauseAndClear();
}

static void ShowPickList(const std::vector<Account>& list, int& choice) {
    clearScreen();
    std::cout << "----- pick account -----\n";
    for (size_t i = 0; i < list.size(); ++i)
        std::cout << i + 1 << ") " << list[i].name << '\n';
    std::cout << list.size() + 1 << ") back\n\nchoice: ";
    std::cin >> choice;
}

bool PickAccount(Account& out) {
    std::vector<Account> list;
    LoadAccounts(list);
    if (list.empty()) {
        std::cout << "No accounts saved yet.\n";
        pauseAndClear();
        return false;
    }
    int c;
    ShowPickList(list, c);
    if (c < 1 || c > static_cast<int>(list.size())) return false;
    out = list[c - 1];
    return true;
}

static void RemoveAccount(std::vector<Account>& list) {
    clearScreen();
    if (list.empty()) {
        std::cout << "No accounts to remove.\n";
        pauseAndClear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i)
        std::cout << i + 1 << ") " << list[i].name << '\n';
    std::cout << list.size() + 1 << ") back\n\nchoice: ";
    int c;  std::cin >> c;
    if (c >= 1 && c <= static_cast<int>(list.size())) {
        list.erase(list.begin() + (c - 1));
        SaveAccounts(list);
        std::cout << "Account removed.\n";
    }
    pauseAndClear();
}

void AccountManagerLoop() {
    std::vector<Account> list;
    LoadAccounts(list);
    while (true) {
        clearScreen();
        std::cout << "----- account manager -----\n"
            << "1) add account\n"
            << "2) remove account\n"
            << "3) back\n\nchoice: ";
        int choice;
        std::cin >> choice;
        if (choice == 3) break;
        if (choice == 1) {
            clearScreen();
            std::cout << "1) add token account\n"
                << "2) back\n\nchoice: ";
            int sub;
            std::cin >> sub;
            if (sub == 1) AddTokenAccount(list);
        }
        if (choice == 2) RemoveAccount(list);
    }
}