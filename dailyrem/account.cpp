#define NOMINMAX
#include "account.h"
#include "functions.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <algorithm>

static const char* TOKEN_FILE = "tokenAccounts.dat";
static const char* MAIL_FILE  = "mailAccounts.dat";
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

static void LoadFromFile(const char* filename, AccountType type, std::vector<Account>& list) {
    list.clear();
    std::ifstream f(filename);
    if (!f) return;
    Account a;
    std::string enc_token;
    a.type = type;
    while (std::getline(f, a.name) && std::getline(f, enc_token)) {
        if (!a.name.empty() && a.name.back() == '\r') a.name.pop_back();
        if (!enc_token.empty() && enc_token.back() == '\r') enc_token.pop_back();
        a.token = XorCipher(FromHex(enc_token));
        list.push_back(a);
        a.type = type;
    }
}

static void SaveToFile(const char* filename, const std::vector<Account>& list) {
    std::ofstream f(filename);
    for (const auto& a : list)
        f << a.name << '\n' << ToHex(XorCipher(a.token)) << '\n';
}

void LoadTokenAccounts(std::vector<Account>& list) { LoadFromFile(TOKEN_FILE, AccountType::TOKEN, list); }
void SaveTokenAccounts(const std::vector<Account>& list) { SaveToFile(TOKEN_FILE, list); }
void LoadMailAccounts(std::vector<Account>& list) { LoadFromFile(MAIL_FILE, AccountType::EMAIL, list); }
void SaveMailAccounts(const std::vector<Account>& list) { SaveToFile(MAIL_FILE, list); }
