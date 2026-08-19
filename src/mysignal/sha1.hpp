#ifndef MYSIGNAL_SHA1_H
#define MYSIGNAL_SHA1_H

#include <cstddef>
#include <cstdint>

namespace mysignal {

const size_t kSha1Size = 20;

void sha1(const uint8_t* data, size_t len, uint8_t out[kSha1Size]);

}

#endif
