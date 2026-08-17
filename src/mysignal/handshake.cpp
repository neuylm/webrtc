#include "handshake.hpp"

#include "base64.hpp"
#include "sha1.hpp"

#include <cstdio>

namespace mysignal {
namespace {

const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

bool sameName(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == '\0' || lower(a[i]) != lower(b[i]))
            return false;
    }
    return b[i] == '\0';
}

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t'))
        begin++;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        end--;
    return s.substr(begin, end - begin);
}

bool mentions(const std::string& haystack, const char* needle) {
    size_t nlen = 0;
    while (needle[nlen] != '\0')
        nlen++;
    if (nlen == 0 || haystack.size() < nlen)
        return false;

    for (size_t i = 0; i + nlen <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < nlen && lower(haystack[i + j]) == lower(needle[j]))
            j++;
        if (j == nlen)
            return true;
    }
    return false;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool base64Charset(const std::string& s) {
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '=';
        if (!ok)
            return false;
    }
    return true;
}

std::string queryField(const std::string& query, const char* name) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && sameName(pair.substr(0, eq), name))
            return percentDecode(pair.substr(eq + 1));
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return std::string();
}

}

std::string percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            int hi = hexDigit(text[i + 1]);
            int lo = hexDigit(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '~';
        if (!ok)
            return false;
    }
    return true;
}

bool sameSecret(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= unsigned(uint8_t(a[i]) ^ uint8_t(b[i]));
    return diff == 0;
}

UpgradeState readUpgrade(const std::string& raw, size_t& consumed, Upgrade& out) {
    size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos)
        return UpgradeState::NeedMore;

    consumed = end + 4;
    out = Upgrade();

    std::string head = raw.substr(0, end + 2);

    size_t lineEnd = head.find("\r\n");
    std::string request = head.substr(0, lineEnd);

    size_t firstSpace = request.find(' ');
    size_t lastSpace = request.rfind(' ');
    if (firstSpace == std::string::npos || lastSpace == firstSpace)
        return UpgradeState::Bad;

    std::string method = request.substr(0, firstSpace);
    std::string target = request.substr(firstSpace + 1, lastSpace - firstSpace - 1);
    std::string version = request.substr(lastSpace + 1);
    if (!sameName(method, "GET") || !sameName(version, "HTTP/1.1"))
        return UpgradeState::Bad;
    if (target.empty() || target[0] != '/')
        return UpgradeState::Bad;

    std::string path = target;
    std::string query;
    size_t mark = target.find('?');
    if (mark != std::string::npos) {
        path = target.substr(0, mark);
        query = target.substr(mark + 1);
    }

    std::string segment = path.substr(1);
    size_t slash = segment.find('/');
    if (slash != std::string::npos)
        segment = segment.substr(0, slash);

    out.id = percentDecode(segment);
    out.token = queryField(query, "token");

    bool upgraded = false;
    bool connected = false;
    bool versioned = false;

    size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        size_t stop = head.find("\r\n", pos);
        if (stop == std::string::npos)
            break;
        std::string line = head.substr(pos, stop - pos);
        pos = stop + 2;
        if (line.empty())
            continue;

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            return UpgradeState::Bad;

        std::string name = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));

        if (sameName(name, "Upgrade"))
            upgraded = mentions(value, "websocket");
        else if (sameName(name, "Connection"))
            connected = mentions(value, "upgrade");
        else if (sameName(name, "Sec-WebSocket-Version"))
            versioned = value == "13";
        else if (sameName(name, "Sec-WebSocket-Key"))
            out.key = value;
    }

    if (!upgraded || !connected || !versioned)
        return UpgradeState::Bad;
    if (out.key.size() != 24 || !base64Charset(out.key))
        return UpgradeState::Bad;

    return UpgradeState::Ok;
}

std::string acceptKey(const std::string& clientKey) {
    std::string material = clientKey + kGuid;
    uint8_t digest[kSha1Size];
    sha1(reinterpret_cast<const uint8_t*>(material.data()), material.size(), digest);
    return base64(digest, kSha1Size);
}

std::string buildAccept(const std::string& clientKey) {
    std::string out;
    out += "HTTP/1.1 101 Switching Protocols\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: " + acceptKey(clientKey) + "\r\n";
    out += "\r\n";
    return out;
}

std::string buildRefusal(int code, const std::string& reason) {
    char line[64];
    std::snprintf(line, sizeof(line), "HTTP/1.1 %d ", code);

    std::string out = line;
    out += reason;
    out += "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    return out;
}

}
