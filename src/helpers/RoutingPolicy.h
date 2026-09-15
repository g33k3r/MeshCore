#pragma once

#include <Packet.h>

namespace mesh {

/**
 * \brief  Test a flood packet against the configured hop limits.
 * \param  packet  inbound flood packet (caller has already checked isRouteFlood())
 * \param  flood_max            max hops for any flood packet
 * \param  flood_max_unscoped   max hops for ROUTE_TYPE_FLOOD (ie. un-scoped) packets
 * \param  flood_max_advert     max hops for ADVERT packets
 * \returns  true if the packet has exceeded a limit, and must not be forwarded
 */
inline bool isFloodHopLimitExceeded(const Packet* packet, uint8_t flood_max,
                                    uint8_t flood_max_unscoped, uint8_t flood_max_advert) {
  uint8_t hops = packet->getPathHashCount();
  if (hops >= flood_max) return true;
  if (packet->getRouteType() == ROUTE_TYPE_FLOOD && hops >= flood_max_unscoped) return true;
  if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT && hops >= flood_max_advert) return true;
  return false;
}

/**
 * \brief  How a server routes a reply back to the requesting client.
 */
enum ReplyRoute : uint8_t {
  REPLY_ROUTE_PATH_RETURN,       // request arrived by flood: reply with a PATH return, flooded back
  REPLY_ROUTE_DIRECT_SUPPLIED,   // reply DIRECT, along the return path supplied in the request
  REPLY_ROUTE_DIRECT_OUT_PATH,   // reply DIRECT, along the out_path already stored for this client
  REPLY_ROUTE_FLOOD,             // no return path known: flood the reply
};

/**
 * \param  inbound_is_flood    the request arrived as a flood packet
 * \param  have_supplied_path  the request payload carried an explicit reply path
 * \param  have_out_path       this server already has a stored out_path for the client
 */
inline ReplyRoute chooseReplyRoute(bool inbound_is_flood, bool have_supplied_path, bool have_out_path) {
  if (inbound_is_flood) return REPLY_ROUTE_PATH_RETURN;
  if (have_supplied_path) return REPLY_ROUTE_DIRECT_SUPPLIED;
  if (have_out_path) return REPLY_ROUTE_DIRECT_OUT_PATH;
  return REPLY_ROUTE_FLOOD;
}

/**
 * \brief  Sentinel for "no SNR measurement available" in path-quality scoring.
 */
static const int PATH_SNR_UNKNOWN = -1000;

/**
 * \brief  Quality score for a return-path candidate: stronger bottleneck SNR
 *         wins (a chain is as reliable as its weakest link), extra hops are
 *         penalized (airtime + failure points per hop).
 * \param  hops              number of forwarder entries in the path
 * \param  bottleneck_snr4   weakest per-hop SNR in SNR*4 units (as recorded by
 *                          TRACE), or PATH_SNR_UNKNOWN when unmeasured
 * \returns  higher is better. Scale: 1 SNR*4 unit (0.25 dB) = 25 points,
 *          1 hop = 100 points — a path must be ~1 dB stronger at the
 *          bottleneck to justify one extra hop.
 */
inline int pathQualityScore(uint8_t hops, int bottleneck_snr4) {
  if (bottleneck_snr4 == PATH_SNR_UNKNOWN) return -((int)hops) * 100;
  return bottleneck_snr4 * 25 - ((int)hops) * 100;
}

/**
 * \brief  Decide whether a newly offered return path should replace the stored one.
 *         Unmeasured SNR on either side degrades to pure hop-count comparison
 *         (the previous first-/last-arrival behavior, minus the randomness).
 * \param  replace_if_equal  tie-break policy (default: keep stored — stability)
 */
inline bool shouldReplacePath(uint8_t new_hops, int new_snr4,
                              uint8_t stored_hops, int stored_snr4,
                              bool replace_if_equal = false) {
  int a, b;
  if (new_snr4 == PATH_SNR_UNKNOWN || stored_snr4 == PATH_SNR_UNKNOWN) {
    a = -((int)new_hops); b = -((int)stored_hops);
  } else {
    a = pathQualityScore(new_hops, new_snr4);
    b = pathQualityScore(stored_hops, stored_snr4);
  }
  return replace_if_equal ? (a >= b) : (a > b);
}

/**
 * \brief  Bottleneck (minimum) per-hop SNR from a completed TRACE's recordings.
 * \param  path_snrs  per-hop SNR*4 bytes, as appended by each forwarder
 * \param  path_len   number of recorded hops
 * \returns  the weakest hop's SNR*4, or PATH_SNR_UNKNOWN when nothing recorded
 */
inline int traceBottleneckSnr4(const uint8_t* path_snrs, uint8_t path_len) {
  if (path_snrs == NULL || path_len == 0) return PATH_SNR_UNKNOWN;
  int bottleneck = 127;
  for (uint8_t i = 0; i < path_len; i++) {
    int v = (int)(int8_t)path_snrs[i];
    if (v < bottleneck) bottleneck = v;
  }
  return bottleneck;
}

/**
 * \brief  Which transport scope a flooded reply should be sent with.
 */
enum ReplyScope : uint8_t {
  REPLY_SCOPE_REQUEST,   // re-use the scope the request arrived on
  REPLY_SCOPE_DEFAULT,   // fall back to this node's default region scope
  REPLY_SCOPE_NONE,      // send un-scoped (ROUTE_TYPE_FLOOD)
};

/**
 * \param  request_scope_known         request arrived scoped, and we resolved its Region's key
 * \param  request_was_unscoped_flood  request arrived as an un-scoped flood
 * \param  default_scope_known         this node has a default Region with a usable transport key
 */
inline ReplyScope chooseReplyScope(bool request_scope_known, bool request_was_unscoped_flood,
                                   bool default_scope_known) {
  if (request_scope_known) return REPLY_SCOPE_REQUEST;
  if (request_was_unscoped_flood) return REPLY_SCOPE_NONE;  // requester chose un-scoped, so mirror it
  if (default_scope_known) return REPLY_SCOPE_DEFAULT;      // scope unknowable: DIRECT, or unresolved Region
  return REPLY_SCOPE_NONE;
}

}
