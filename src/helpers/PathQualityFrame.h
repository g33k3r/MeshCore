#pragma once
// Private-dialect path-quality tail for contact frames (g33k3r fork).
//
// Gate: app_target_ver >= PATHQ_MIN_APP_VER (90). Upstream firmware versions
// are single-digit-to-teens and will never legitimately reach 90, so stock
// clients and upstream firmware never see these bytes; old clients that do
// not send v90 never receive them (their parsers would misread the tail as
// gps/lastmod — hence the hard version gate, same mechanism upstream used
// for isRepeatEn @v9 and path_hash_mode @v10).
//
// Layout (3 bytes, appended after lastmod):
//   [int16 LE path_snr4][uint8 quality_flags]
//     path_snr4     measured bottleneck SNR*4 of the contact's route
//                   (PATH_SNR_UNKNOWN = -1000 when unmeasured)
//     quality_flags bit0: alternate path available for this contact
#include <helpers/ContactInfo.h>
#include <helpers/RoutingPolicy.h>
#include <string.h>

#define PATHQ_MIN_APP_VER   90      // 3-byte tail (snr + flags)
#define PATHQ_ALT_MIN_APP_VER 91   // v91: + [u8 alt_len][alt path bytes]
#define PATHQ_FLAG_HAS_ALT  0x01

inline int appendPathQualityTail(uint8_t* buf, const ContactInfo& contact) {
  int16_t v = contact.path_snr4;
  memcpy(buf, &v, 2);
  buf[2] = (contact.alt_path_len != OUT_PATH_UNKNOWN) ? PATHQ_FLAG_HAS_ALT : 0;
  return 3;
}

inline void parsePathQualityTail(const uint8_t* buf, int16_t& snr4, uint8_t& flags) {
  memcpy(&snr4, buf, 2);
  flags = buf[2];
}

// v91 extended tail: [i16 snr4][u8 flags][u8 alt_len][alt hash bytes].
// 'max_alt_bytes' enforces the frame budget (contact frame is 148 + 4 + alt;
// MAX_FRAME_SIZE 176 => alt <= 24). When the alt path doesn't fit, it is
// omitted (flag still set) — receivers treat presence of the len byte as the
// discriminator between v90 and v91 tails.
// Returns total bytes written (3 for flag-only, 4+n with bytes).
inline int appendPathQualityTailEx(uint8_t* buf, const ContactInfo& contact, uint8_t max_alt_bytes) {
  int16_t v = contact.path_snr4;
  memcpy(buf, &v, 2);
  buf[2] = (contact.alt_path_len != OUT_PATH_UNKNOWN) ? PATHQ_FLAG_HAS_ALT : 0;
  if (contact.alt_path_len == OUT_PATH_UNKNOWN) return 3;
  uint8_t count = contact.alt_path_len & 63;
  uint8_t sz = ((contact.alt_path_len >> 6) & 3) + 1;
  uint8_t n = (uint8_t)(count * sz);
  if (n == 0 || n > max_alt_bytes) return 3;   // doesn't fit: flag-only
  buf[3] = contact.alt_path_len;               // encoded byte (count+size)
  memcpy(&buf[4], contact.alt_path, n);
  return 4 + n;
}
