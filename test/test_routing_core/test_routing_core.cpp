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
