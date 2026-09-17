#pragma once

// Link-adaptive LoRa coding rate (fork feature, roadmap: "dynamic CR for weak
// vs strong hops").
//
// LoRa encodes CR in the explicit PHY header of every packet, so receivers
// (stock nodes included) decode any CR we transmit — raising CR on a weak
// link buys ~+1 dB of link budget per step for ~+20% payload airtime, with
// zero protocol changes. Preamble/CAD are CR-independent, and the RX-side
// header-timeout windows are already worst-cased to the highest CR
// (RadioLibWrapper::calcMaxPacketMillis), so nothing else needs to move.
//
// Input is the contact's measured link SNR: for direct-neighbour contacts
// path_snr4 is refreshed from every received PEER_PATH packet, so it is the
// freshest real measurement of the exact link a DIRECT send will use.
//
// Returns a CR in 5..8, or 0 meaning "use the radio's configured default"
// (unmeasured / stale / strong link). Stateless by design: CR is a per-packet
// property, so band-edge flapping is harmless (airtime estimates track the
// configured CR via getTimeOnAir()).

#include "RoutingPolicy.h"

namespace mesh {

// Margins in dB above the spreading factor's demod floor (snr_threshold table).
static const float ADAPTIVE_CR_ROBUST_BELOW_MARGIN = 6.0f;   // weaker than this: CR 4/8
static const float ADAPTIVE_CR_MID_BELOW_MARGIN = 12.0f;     // weaker than this: CR 4/6
static const uint32_t ADAPTIVE_CR_FRESH_SECS = 600;          // older measurement = unmeasured

/**
 * \brief  Pick a coding rate for a DIRECT send over a measured link.
 * \param  path_snr4       measured bottleneck SNR in SNR*4 units (PATH_SNR_UNKNOWN = unmeasured)
 * \param  demod_floor_db  demodulation floor of the current SF (radio's snr_threshold), dB
 * \param  age_secs        seconds since the measurement was taken
 * \returns 0 (configured default), or CR denominator 5..8. Higher = more robust.
 */
inline uint8_t adaptiveCodingRate(int path_snr4, float demod_floor_db, uint32_t age_secs) {
  if (path_snr4 == PATH_SNR_UNKNOWN) return 0;
  if (age_secs > ADAPTIVE_CR_FRESH_SECS) return 0;
  float margin_db = (path_snr4 * 0.25f) - demod_floor_db;
  if (margin_db < ADAPTIVE_CR_ROBUST_BELOW_MARGIN) return 8;
  if (margin_db < ADAPTIVE_CR_MID_BELOW_MARGIN) return 6;
  return 0;
}

}
