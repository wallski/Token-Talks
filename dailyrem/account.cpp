#define NOMINMAX
#include "account.h"
#include "functions.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <algorithm>

static const char* FILE_NAME = "accounts.dat";
static const unsigned char XOR_KEY[] = { 0x54, 0x4B, 0x56, 0x61, 0x75, 0x6C, 0x74, 0x21 };

static std::string XorCipher(const std::string& in) {
    std::string out = in;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] ^= XOR_KEY[i % sizeof(XOR_KEY)];
    return out;
}

static std::string ToHex(const std::string& in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
    }
    return out;
}

static std::string FromHex(const std::string& in) {
    std::string out;
    out.reserve(in.size() / 2);
    for (size_t i = 0; i + 1 < in.size(); i += 2) {
        auto hexVal = [](char c) -> unsigned char {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        out += (char)((hexVal(in[i]) << 4) | hexVal(in[i+1]));
    }
    return out;
}

void LoadAccounts(std::vector<Account>& list) {
    list.clear();
    std::ifstream f(FILE_NAME);
    if (!f) return;
    Account a;
    std::string enc_token;
    while (std::getline(f, a.name) && std::getline(f, enc_token)) {
        if (!a.name.empty() && a.name.back() == '\r') a.name.pop_back();
        if (!enc_token.empty() && enc_token.back() == '\r') enc_token.pop_back();
        a.token = XorCipher(FromHex(enc_token));
        list.push_back(a);
    }
}

void SaveAccounts(const std::vector<Account>& list) {
    std::ofstream f(FILE_NAME);
    for (const auto& a : list)
        f << a.name << '\n' << ToHex(XorCipher(a.token)) << '\n';
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