// Characterization tests for the MeshCore routing core (src/Mesh.cpp).
// These encode CURRENT behavior of flood routing, direct routing, duplicate
// suppression, and forwarding gates, via a virtual radio mesh — no hardware.
#include <gtest/gtest.h>
#include "harness.h"

using meshcore_test::Node;
using meshcore_test::run;
using meshcore_test::step;

class RoutingCore : public ::testing::Test {
protected:
  void SetUp() override {
    a = std::make_unique<Node>(0xAA);
    b = std::make_unique<Node>(0xBB);
    c = std::make_unique<Node>(0xCC);
    for (auto* n : {a.get(), b.get(), c.get()}) n->mesh.begin();
  }
  std::unique_ptr<Node> a, b, c;
};

// ── Flood routing ──────────────────────────────────────────────

TEST_F(RoutingCore, FloodAdvertPropagatesAndDeliversToAllNodes) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);

  run({a.get(), b.get(), c.get()});

  EXPECT_EQ(a->mesh.adverts, 0);  // sender does not deliver its own advert to itself
  EXPECT_EQ(b->mesh.adverts, 1);
  EXPECT_EQ(c->mesh.adverts, 1);
  EXPECT_GE(b->mesh.getNumRecvFlood(), 1u);
  EXPECT_GE(c->mesh.getNumRecvFlood(), 1u);
}

TEST_F(RoutingCore, FloodForwardsExactlyOncePerNode) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  // B forwarded the flood exactly once (its rebroadcast also reaches C,
  // and C's rebroadcast reaches B again — which must be suppressed).
  EXPECT_EQ(b->radio.sent, 1u);
  EXPECT_EQ(c->mesh.adverts, 1);
}

TEST_F(RoutingCore, DuplicateFloodArrivalIsSuppressed) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  // After convergence, B has heard its own rebroadcast come back from C.
  EXPECT_GE(b->tables.getNumFloodDups(), 1u);
  EXPECT_EQ(b->mesh.adverts, 1);  // delivered exactly once despite re-hearing
}

TEST_F(RoutingCore, ForwardGateOffStopsFloodPropagation) {
  a->connect(*b);
  b->connect(*c);
  b->mesh.forward_enabled = false;  // B is a Companion (non-repeating) node

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  EXPECT_EQ(b->mesh.adverts, 1);   // B still receives...
  EXPECT_EQ(b->radio.sent, 0u);  // ...but never retransmits
  EXPECT_EQ(c->mesh.adverts, 0);   // ...so C hears nothing
}

// ── Direct routing ─────────────────────────────────────────────

TEST_F(RoutingCore, DirectDatagramToNeighborDeliversZeroHop) {
  a->connect(*b);
  a->mesh.known_peer = false;
  b->mesh.known_peer = true;  // B recognizes A as a known peer
  memset(b->mesh.shared_secret, 0x5A, PUB_KEY_SIZE);

  const uint8_t msg[] = "hello mesh";
  auto* pkt = a->mesh.createDatagram(PAYLOAD_TYPE_TXT_MSG, b->mesh.self_id,
                                     b->mesh.shared_secret, msg, sizeof(msg));
  ASSERT_NE(pkt, nullptr);
  // B is a direct neighbor: no forwarders needed
  a->mesh.sendDirect(pkt, nullptr, 0, 0);
  run({a.get(), b.get()});

  EXPECT_EQ(b->mesh.peer_msgs, 1);
  // Deterministic mocks: decrypted payload = message, zero-padded to the
  // 16-byte cipher block (identity AES). Verified byte-exact.
  ASSERT_EQ(b->mesh.last_peer_data.size(), ((sizeof(msg) + 15) / 16) * 16);
  EXPECT_EQ(memcmp(b->mesh.last_peer_data.data(), msg, sizeof(msg)), 0);
}

TEST_F(RoutingCore, DirectDatagramRoutesThroughForwarder) {
  // Topology: A --- B --- C  (A and C NOT in range of each other)
  a->connect(*b);
  b->connect(*c);
  c->mesh.known_peer = true;
  memset(c->mesh.shared_secret, 0x5A, PUB_KEY_SIZE);

  const uint8_t msg[] = "multi-hop";
  auto* pkt = a->mesh.createDatagram(PAYLOAD_TYPE_TXT_MSG, c->mesh.self_id,
                                     c->mesh.shared_secret, msg, sizeof(msg));
  ASSERT_NE(pkt, nullptr);
  uint8_t path[1];
  b->mesh.self_id.copyHashTo(path, 1);  // B is the forwarder
  a->mesh.sendDirect(pkt, path, 1, 0);
  run({a.get(), b.get(), c.get()});

  // B forwards exactly once (direct traffic)
  EXPECT_EQ(b->mesh.getNumSentDirect(), 1u);
  // C receives the datagram addressed to it
  EXPECT_EQ(c->mesh.peer_msgs, 1);
  // Deterministic mocks: decrypted payload = message, zero-padded to the
  // 16-byte cipher block (identity AES). Verified byte-exact.
  ASSERT_EQ(c->mesh.last_peer_data.size(), ((sizeof(msg) + 15) / 16) * 16);
  EXPECT_EQ(memcmp(c->mesh.last_peer_data.data(), msg, sizeof(msg)), 0);
}

// ── ACK routing ────────────────────────────────────────────────

TEST_F(RoutingCore, FloodAckDeliversCrcToRecipient) {
  a->connect(*b);

  auto* pkt = a->mesh.createAck((uint32_t)0xDEADBEEF);
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get()});

  EXPECT_EQ(b->mesh.acks, 1);
  EXPECT_EQ(b->mesh.last_ack_crc, 0xDEADBEEFu);
}

// ── TRACE routing ──────────────────────────────────────────────

TEST_F(RoutingCore, TraceTerminatesAtEndOfPath) {
  a->connect(*b);

  auto* pkt = a->mesh.createTrace(0x12345678u, 0x9ABCDEF0u);
  ASSERT_NE(pkt, nullptr);
  // Empty path in payload: first receiver is the terminal node
  a->mesh.sendDirect(pkt, nullptr, 0, 0);
  run({a.get(), b.get()});

  EXPECT_EQ(b->mesh.traces, 1);
}

// ── Loss resilience ────────────────────────────────────────────

TEST_F(RoutingCore, DroppedTransmissionDoesNotDuplicateDelivery) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  a->mesh.sendFlood(pkt, 0u);

  // One transmission gets swallowed; the mesh still converges
  b->radio.drop_next = true;
  run({a.get(), b.get(), c.get()});

  // B's first forward attempt vanished; A's original (already delivered
  // before the drop took effect on B's rebroadcast) still reached B once.
  EXPECT_LE(b->mesh.adverts, 1);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


// ── Group channel routing ──────────────────────────────────────

TEST_F(RoutingCore, GroupDatagramFloodDeliversToChannelMembers) {
  a->connect(*b);
  b->mesh.known_channel = true;
  memset(a->mesh.channel.hash, 0x99, PATH_HASH_SIZE);
  memset(a->mesh.channel.secret, 0x77, PUB_KEY_SIZE);
  b->mesh.channel = a->mesh.channel;

  const uint8_t msg[] = "group hello";
  auto* pkt = a->mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, a->mesh.channel,
                                          msg, sizeof(msg));
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get()});

  EXPECT_EQ(b->mesh.group_msgs, 1);
  // identity cipher: content present, block-padded
  ASSERT_GE(b->mesh.last_group_data.size(), sizeof(msg));
  EXPECT_EQ(memcmp(b->mesh.last_group_data.data(), msg, sizeof(msg)), 0);
}

// ── Anonymous datagrams ────────────────────────────────────────

TEST_F(RoutingCore, AnonDatagramDeliversAndStopsAtRecipient) {
  a->connect(*b);
  b->connect(*c);

  const uint8_t msg[] = "anon ping";
  uint8_t anon_secret[PUB_KEY_SIZE] = {0};   // mock calcSharedSecret is constant-zero
  auto* pkt = a->mesh.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, a->mesh.self_id,
                                         b->mesh.self_id, anon_secret,
                                         msg, sizeof(msg));
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  // delivered to the addressed recipient
  EXPECT_EQ(b->mesh.anon_msgs, 1);
  ASSERT_GE(b->mesh.last_anon_data.size(), sizeof(msg));
  EXPECT_EQ(memcmp(b->mesh.last_anon_data.data(), msg, sizeof(msg)), 0);
  // successful anon decrypt marks do-not-retransmit: recipient does not forward
  EXPECT_EQ(b->radio.sent, 0u);
}

// ── Multipart ACK relay ────────────────────────────────────────

TEST_F(RoutingCore, MultipartAckRelayedThroughForwarder) {
  a->connect(*b);

  auto* pkt = a->mesh.createMultiAck((uint32_t)0xCAFEBABEu, 2);
  ASSERT_NE(pkt, nullptr);
  uint8_t path[1];
  b->mesh.self_id.copyHashTo(path, 1);
  a->mesh.sendDirect(pkt, path, 1, 0);
  run({a.get(), b.get()});

  // B relayed the embedded ACK (zero-hop rebroadcast back to A)
  EXPECT_GE(b->mesh.getNumSentDirect(), 1u);
  EXPECT_EQ(a->mesh.acks, 1);
  EXPECT_EQ(a->mesh.last_ack_crc, 0xCAFEBABEu);
}

// ── Transport-coded flood ──────────────────────────────────────

TEST_F(RoutingCore, TransportCodedFloodForwardsWithCodesIntact) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  ASSERT_NE(pkt, nullptr);
  uint16_t codes[2] = {0x1234, 0x5678};
  a->mesh.sendFlood(pkt, codes, 0u);
  run({a.get(), b.get(), c.get()});

  EXPECT_EQ(c->mesh.adverts, 1);   // propagated through B
  // B's rebroadcast preserves the transport codes in the frame
  ASSERT_FALSE(b->radio.last_tx.empty());
  uint8_t hdr = b->radio.last_tx[0];
  EXPECT_EQ(hdr & PH_ROUTE_MASK, ROUTE_TYPE_TRANSPORT_FLOOD);
  uint16_t c0 = (uint16_t)b->radio.last_tx[1] | ((uint16_t)b->radio.last_tx[2] << 8);
  uint16_t c1 = (uint16_t)b->radio.last_tx[3] | ((uint16_t)b->radio.last_tx[4] << 8);
  EXPECT_EQ(c0, 0x1234);
  EXPECT_EQ(c1, 0x5678);
}

// ── Zero-hop scope ─────────────────────────────────────────────

TEST_F(RoutingCore, ZeroHopSendReachesNeighborsOnly) {
  a->connect(*b);
  b->connect(*c);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendZeroHop(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  EXPECT_EQ(b->mesh.adverts, 1);   // neighbor receives...
  EXPECT_EQ(b->radio.sent, 0u);    // ...and never retransmits
  EXPECT_EQ(c->mesh.adverts, 0);   // ...so nothing propagates further
}


// ── Alternate-path learning ────────────────────────────────────

TEST_F(RoutingCore, DuplicateFloodViaDifferentRouteYieldsAltPath) {
  // Diamond topology: A reaches B directly AND via C
  a->connect(*b);
  a->connect(*c);
  c->connect(*b);

  auto* pkt = a->mesh.createAdvert(a->mesh.self_id);
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  // B received the advert once (direct copy wins delivery)...
  EXPECT_EQ(b->mesh.adverts, 1);
  // ...and C's forwarded duplicate fired the alternate-path hook with C's route
  EXPECT_EQ(b->mesh.alt_paths, 1);
  ASSERT_EQ(b->mesh.last_alt_path_len, 1u);
  EXPECT_EQ(b->mesh.last_alt_path[0], 0xCC);  // C's hash prefix
}


TEST_F(RoutingCore, DuplicateMessageFloodYieldsAltPath) {
  // Diamond topology with an addressed message: A sends TXT_MSG to B,
  // directly AND via C — the dup via C must surface C's route.
  a->connect(*b);
  a->connect(*c);
  c->connect(*b);
  b->mesh.known_peer = true;
  memset(b->mesh.shared_secret, 0x5A, PUB_KEY_SIZE);

  const uint8_t msg[] = "route me";
  auto* pkt = a->mesh.createDatagram(PAYLOAD_TYPE_TXT_MSG, b->mesh.self_id,
                                     b->mesh.shared_secret, msg, sizeof(msg));
  ASSERT_NE(pkt, nullptr);
  a->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get(), c.get()});

  EXPECT_EQ(b->mesh.peer_msgs, 1);        // delivered once
  EXPECT_EQ(b->mesh.alt_paths, 1);        // C's route captured from the dup
  ASSERT_EQ(b->mesh.last_alt_path_len, 1u);
  EXPECT_EQ(b->mesh.last_alt_path[0], 0xCC);
}


TEST_F(RoutingCore, DualPathReplyExtraSurvivesTheWire) {
  a->connect(*b);
  b->mesh.known_peer = true;
  a->mesh.known_peer = true;
  memset(b->mesh.shared_secret, 0x5A, PUB_KEY_SIZE);
  memset(a->mesh.shared_secret, 0x5A, PUB_KEY_SIZE);

  // B replies to A with a primary path + alt-path extra (as the repeater now does)
  uint8_t primary_path[1] = {0xCC};          // "via C"
  uint8_t alt_extra[2] = {0x01, 0xDD};       // encoded path_len (count=1,size=1) + alt hash byte
  mesh::Packet* pkt = b->mesh.createPathReturn(a->mesh.self_id, b->mesh.shared_secret,
                                               primary_path, 1, PATH_EXTRA_TYPE_ALT_PATH,
                                               alt_extra, sizeof(alt_extra));
  ASSERT_NE(pkt, nullptr);
  b->mesh.sendFlood(pkt, 0u);
  run({a.get(), b.get()});

  // A received the PATH packet with the alt extra intact through the full
  // encrypt/decrypt round-trip
  EXPECT_EQ(a->mesh.path_pkts, 1);
  EXPECT_EQ(a->mesh.alt_extras, 1);
  // Wire quirk (characterized): extra_len includes cipher-block zero padding —
  // readers parse their own length fields and ignore the tail.
  ASSERT_GE(a->mesh.last_path_extra.size(), 2u);
  EXPECT_EQ(a->mesh.last_path_extra[0], 0x01);   // encoded alt path_len
  EXPECT_EQ(a->mesh.last_path_extra[1], 0xDD);   // the alt forwarder hash
}
