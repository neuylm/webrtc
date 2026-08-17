#ifndef MYSIGNAL_BASE64_H
#define MYSIGNAL_BASE64_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mysignal {

std::string base64(const uint8_t* data, size_t len);

}

#endif
