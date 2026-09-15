#pragma once
// Mock base64.hpp (aedocw/Arduino style) for native testing — used by
// BaseChatMesh channel add; deterministic passthrough, length returned.
#include <stddef.h>
#include <string.h>
inline size_t encode_base64(const unsigned char* src, size_t len, unsigned char* dst) {
  if (dst && src) memcpy(dst, src, len);
  return len;
}
inline size_t decode_base64(const unsigned char* src, size_t len, unsigned char* dst) {
  if (dst && src) memcpy(dst, src, len);
  return len;
}
