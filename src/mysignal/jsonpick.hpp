#ifndef MYSIGNAL_JSONPICK_H
#define MYSIGNAL_JSONPICK_H

#include <cstddef>
#include <string>

namespace mysignal {

struct Span {
    size_t begin = 0;
    size_t end = 0;
};

bool pickTopString(const std::string& text, const std::string& key, std::string& value, Span& span);
std::string quoteJson(const std::string& value);
std::string splice(const std::string& text, const Span& span, const std::string& piece);

}

#endif
