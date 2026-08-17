#include "jsonpick.hpp"

#include <cstdint>
#include <cstdio>

namespace mysignal {
namespace {

void skipBlanks(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
}

bool readString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"')
        return false;
    i++;

    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            i++;
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= s.size())
                return false;
            char esc = s[i + 1];
            i += 2;
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (i + 4 > s.size())
                    return false;
                out.append(s, i - 2, 6);
                i += 4;
                break;
            default:
                return false;
            }
            continue;
        }
        out.push_back(c);
        i++;
    }
    return false;
}

bool skipValue(const std::string& s, size_t& i, bool& wasString) {
    skipBlanks(s, i);
    if (i >= s.size())
        return false;

    wasString = s[i] == '"';
    if (wasString) {
        std::string ignored;
        return readString(s, i, ignored);
    }

    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        bool inString = false;
        while (i < s.size()) {
            char c = s[i];
            if (inString) {
                if (c == '\\') {
                    i += 2;
                    continue;
                }
                if (c == '"')
                    inString = false;
                i++;
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) {
                    i++;
                    return true;
                }
            }
            i++;
        }
        return false;
    }

    size_t start = i;
    while (i < s.size()) {
        char c = s[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        i++;
    }
    return i > start;
}

}

bool pickTopString(const std::string& text, const std::string& key, std::string& value, Span& span) {
    size_t i = 0;
    skipBlanks(text, i);
    if (i >= text.size() || text[i] != '{')
        return false;
    i++;

    while (true) {
        skipBlanks(text, i);
        if (i >= text.size())
            return false;
        if (text[i] == '}')
            return false;

        std::string name;
        if (!readString(text, i, name))
            return false;

        skipBlanks(text, i);
        if (i >= text.size() || text[i] != ':')
            return false;
        i++;

        skipBlanks(text, i);
        size_t valueBegin = i;
        bool wasString = false;
        if (!skipValue(text, i, wasString))
            return false;

        if (name == key) {
            if (!wasString)
                return false;
            span.begin = valueBegin;
            span.end = i;

            size_t reread = valueBegin;
            return readString(text, reread, value);
        }

        skipBlanks(text, i);
        if (i >= text.size())
            return false;
        if (text[i] == ',') {
            i++;
            continue;
        }
        if (text[i] == '}')
            return false;
        return false;
    }
}

std::string quoteJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');

    for (char c : value) {
        uint8_t raw = uint8_t(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (raw < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", unsigned(raw));
            out += esc;
        } else {
            out.push_back(c);
        }
    }

    out.push_back('"');
    return out;
}

std::string splice(const std::string& text, const Span& span, const std::string& piece) {
    if (span.end > text.size() || span.begin > span.end)
        return text;
    return text.substr(0, span.begin) + piece + text.substr(span.end);
}

}
