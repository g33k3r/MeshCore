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

#define PATHQ_MIN_APP_VER   90
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
