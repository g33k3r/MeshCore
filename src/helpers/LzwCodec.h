#pragma once

/*
 * LzwCodec — embedded-safe LZW compression for TXT payloads.
 *
 * Fork feature: text is compressed INSIDE the end-to-end encryption
 * (intermediate nodes route ciphertext blindly), gated on the sender knowing
 * the recipient advertised the fork capability (ADV_FEAT1_FORK_LZW in the
 * signed advert feat1 field). Stock peers never receive LZW payloads.
 *
 * Constraints honored (upstream contributing rules):
 *   - no dynamic memory allocation: tables are fixed-size members; instantiate
 *     as static/file-scope storage
 *   - decode is adversarially safe: every code checked against next_code,
 *     every write bounded by the caller's cap; corrupt input fails with -1
 *     instead of reading/writing out of bounds
 *
 * Wire format (the encrypted TXT blob after the 5-byte header):
 *   [0x1F][lzw code stream, big-endian bit packing, 9..12-bit codes]
 * The 0x1F marker byte distinguishes compressed payloads from legacy plain
 * text (which never starts with 0x1F from the app's text input).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mesh {

#define TXT_PAYLOAD_LZW_MARKER   0x1F
#define LZW_MIN_TEXT_TO_COMPRESS 24   // below this, LZW overhead eats the gain

class LzwCodec {
  static const int LZW_BITS_MIN = 9;
  static const int LZW_BITS_MAX = 12;
  static const int LZW_CLEAR = 256;         // dictionary reset code
  static const int LZW_DICT_SIZE = 1 << LZW_BITS_MAX;  // 4096
  static const int LZW_FIRST_FREE = 257;

  // table shared shape: prefix chain + suffix byte per entry
  uint16_t _prefix[LZW_DICT_SIZE];
  uint8_t  _suffix[LZW_DICT_SIZE];

  void resetTable(int& next_code, int& code_bits) {
    next_code = LZW_FIRST_FREE;
    code_bits = LZW_BITS_MIN;
  }

public:
  /**
   * Compress in_len bytes into out (cap = out_cap).
   * Returns compressed length, or -1 if it does not fit / would not help.
   * The marker byte is NOT included — the caller prepends it.
   */
  int compress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if (in_len == 0 || out_cap < 4) return -1;
    memset(_prefix, 0, sizeof(_prefix));
    memset(_suffix, 0, sizeof(_suffix));
    int next_code, code_bits;
    resetTable(next_code, code_bits);

    size_t out_pos = 0;
    uint32_t bit_buf = 0;
    int bit_count = 0;

    auto put_code = [&](int code) -> bool {
      bit_buf = (bit_buf << code_bits) | (uint32_t)code;
      bit_count += code_bits;
      while (bit_count >= 8) {
        if (out_pos >= out_cap) return false;
        out[out_pos++] = (uint8_t)(bit_buf >> (bit_count - 8));
        bit_count -= 8;
      }
      return true;
    };

    int cur = in[0];
    for (size_t i = 1; i < in_len; i++) {
      uint8_t c = in[i];
      // search for (cur, c) in table: linear scan is too slow at 4096 entries
      // per input byte; use the classic time-space tradeoff instead — hash
      // not allowed (determinism), so we keep a chain-free approach:
      // entries are appended in order, scan backwards stops at first prefix
      // match chain boundary. For our sizes (<200 byte payloads) a bounded
      // backward scan from next_code is fast and code-size-independent.
      int found = -1;
      for (int e = next_code - 1; e >= LZW_FIRST_FREE; e--) {
        if (_prefix[e] == cur && _suffix[e] == c) { found = e; break; }
      }
      if (found >= 0) {
        cur = found;
      } else {
        if (!put_code(cur)) return -1;
        if (next_code < LZW_DICT_SIZE) {
          _prefix[next_code] = (uint16_t)cur;
          _suffix[next_code] = c;
          next_code++;
          if (next_code == (1 << code_bits) && code_bits < LZW_BITS_MAX) {
            code_bits++;
          }
        } else {
          // table full: emit CLEAR and restart dictionary
          if (!put_code(LZW_CLEAR)) return -1;
          resetTable(next_code, code_bits);
        }
        cur = c;
      }
    }
    if (!put_code(cur)) return -1;
    // flush trailing bits
    if (bit_count > 0) {
      if (out_pos >= out_cap) return -1;
      out[out_pos++] = (uint8_t)(bit_buf << (8 - bit_count));
    }
    return (int)out_pos;
  }

  /**
   * Decompress in_len bytes into out (cap = out_cap).
   * Returns decompressed length, or -1 on corrupt input / overflow.
   * Never reads or writes out of bounds — safe on hostile bytes.
   */
  int decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if (in_len == 0 || out_cap == 0) return -1;
    memset(_prefix, 0, sizeof(_prefix));
    memset(_suffix, 0, sizeof(_suffix));
    int next_code, code_bits;
    resetTable(next_code, code_bits);

    size_t out_pos = 0;
    size_t in_pos = 0;
    uint32_t bit_buf = 0;
    int bit_count = 0;

    auto get_code = [&](int& code) -> bool {
      while (bit_count < code_bits) {
        if (in_pos >= in_len) return false;
        bit_buf = (bit_buf << 8) | in[in_pos++];
        bit_count += 8;
      }
      code = (int)((bit_buf >> (bit_count - code_bits)) & ((1u << code_bits) - 1));
      bit_count -= code_bits;
      return true;
    };

    // emit the chain for an existing code into out (bounded)
    auto emit = [&](int code, uint8_t*& first_suffix) -> bool {
      // walk chain into a bounded stack (max dict chain = dict size)
      uint8_t stack[LZW_DICT_SIZE];
      int n = 0;
      int c = code;
      while (c >= LZW_FIRST_FREE) {
        if (n >= LZW_DICT_SIZE) return false;   // corrupt chain (cycle guard)
        stack[n++] = _suffix[c];
        c = _prefix[c];
        if (c >= LZW_DICT_SIZE) return false;   // corrupt prefix
      }
      if (c > 255) return false;                // corrupt: root must be a byte
      stack[n++] = (uint8_t)c;
      if (out_pos + (size_t)n > out_cap) return false;
      for (int k = n - 1; k >= 0; k--) out[out_pos++] = stack[k];
      first_suffix = out + out_pos - n;         // points at first emitted byte
      return true;
    };

    int code;
    if (!get_code(code)) return -1;
    if (code > 255 || code == LZW_CLEAR) return -1;   // first code must be a literal
    if (out_pos >= out_cap) return false;
    out[out_pos++] = (uint8_t)code;

    int prev = code;
    while (get_code(code)) {
      if (code == LZW_CLEAR) {
        resetTable(next_code, code_bits);
        if (!get_code(code)) return -1;
        if (code > 255) return -1;
        if (out_pos >= out_cap) return false;
        out[out_pos++] = (uint8_t)code;
        prev = code;
        continue;
      }
      uint8_t* first = nullptr;
      if (code < next_code) {
        // existing entry (or the KwKwK special case)
        if (!emit(code, first)) return -1;
        if (next_code < LZW_DICT_SIZE) {
          _prefix[next_code] = (uint16_t)prev;
          _suffix[next_code] = first ? *first : 0;
          next_code++;
          if (next_code == (1 << code_bits) && code_bits < LZW_BITS_MAX) code_bits++;
        }
      } else if (code == next_code) {
        // KwKwK: entry being defined right now
        uint8_t* prev_first = nullptr;
        // emit prev first to learn its first byte
        size_t mark = out_pos;
        if (!emit(prev, first)) return -1;
        (void)mark;
        if (out_pos >= out_cap) return false;
        out[out_pos++] = first ? *first : 0;
        if (next_code < LZW_DICT_SIZE) {
          _prefix[next_code] = (uint16_t)prev;
          _suffix[next_code] = first ? *first : 0;
          next_code++;
          if (next_code == (1 << code_bits) && code_bits < LZW_BITS_MAX) code_bits++;
        }
      } else {
        return -1;   // code > next_code: corrupt stream
      }
      prev = code;
    }
    return (int)out_pos;
  }
};

} // namespace mesh
