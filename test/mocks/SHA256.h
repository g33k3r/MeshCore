#pragma once

#include <stdint.h>
#include <stddef.h>

// Mock SHA256 for native testing — deterministic but not cryptographic.
// finalize() writes real (non-garbage) output so calculatePacketHash() produces
// distinguishable results for packets with different payloads.
#include <string.h>

class SHA256 {
  uint8_t _state[32];
  size_t _len;
public:
  SHA256() : _len(0) { memset(_state, 0, sizeof(_state)); }

  void update(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
      uint8_t b = bytes[i];
      _state[_len % 32] ^= b;
      _state[(_len + 1) % 32] += (uint8_t)((b >> 1) | (b << 7));
      _len++;
    }
  }

  void finalize(uint8_t* hash, size_t hashLen) {
    for (size_t i = 0; i < hashLen; i++) {
      hash[i] = _state[i % 32];
    }
  }

  // Deterministic HMAC stand-in: same (key, data) => same MAC on both sides.
  void resetHMAC(const uint8_t* key, size_t keyLen) {
    memset(_state, 0, sizeof(_state));
    _len = 0;
    update(key, keyLen);  // fold key in first (inner-pad-ish)
  }
  void finalizeHMAC(const uint8_t* key, size_t keyLen, uint8_t* hash, size_t hashLen) {
    // Fold key again (outer-pad-ish) over a copy, then emit — deterministic
    // and symmetric for identical (key, accumulated-data) inputs.
    uint8_t tmp[32];
    memcpy(tmp, _state, sizeof(tmp));
    SHA256 outer;
    outer.update(key, keyLen);
    outer.update(tmp, sizeof(tmp));
    outer.finalize(hash, hashLen);
  }
};
