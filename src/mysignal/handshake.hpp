#ifndef MYSIGNAL_HANDSHAKE_H
#define MYSIGNAL_HANDSHAKE_H

#include <string>

namespace mysignal {

struct Upgrade {
    std::string id;
    std::string token;
    std::string key;
};

enum class UpgradeState { NeedMore, Ok, Bad };

UpgradeState readUpgrade(const std::string& raw, size_t& consumed, Upgrade& out);

std::string acceptKey(const std::string& clientKey);
std::string buildAccept(const std::string& clientKey);
std::string buildRefusal(int code, const std::string& reason);

bool validId(const std::string& id);
bool sameSecret(const std::string& a, const std::string& b);
std::string percentDecode(const std::string& text);

}

#endif
