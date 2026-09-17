// Link-adaptive coding rate (fork feature: dynamic CR for weak vs strong hops).
// Part 1: pure policy bands. Part 2: end-to-end sends through the real Dispatcher
// TX path, asserting the radio was asked for the expected CR.
#include <gtest/gtest.h>
#include "helpers/CodingRatePolicy.h"
#include "harness.h"
#include "helpers/BaseChatMesh.h"
#include <string.h>

using namespace meshcore_test;

// ---------------- policy ----------------

TEST(AdaptiveCR, UnmeasuredIsDefault) {
  EXPECT_EQ(mesh::adaptiveCodingRate(mesh::PATH_SNR_UNKNOWN, -15.0f, 0), 0);
}

TEST(AdaptiveCR, StaleMeasurementIsDefault) {
  EXPECT_EQ(mesh::adaptiveCodingRate(-40, -15.0f, mesh::ADAPTIVE_CR_FRESH_SECS + 1), 0);
}

TEST(AdaptiveCR, FreshBoundaryStillApplies) {
  EXPECT_EQ(mesh::adaptiveCodingRate(-40, -15.0f, mesh::ADAPTIVE_CR_FRESH_SECS), 8);
}

TEST(AdaptiveCR, WeakLinkGoesRobust) {
  // -10 dB SNR at a -15 dB floor = 5 dB margin < 6 -> CR 4/8
  EXPECT_EQ(mesh::adaptiveCodingRate(-40, -15.0f, 0), 8);
  EXPECT_EQ(mesh::adaptiveCodingRate(-100, -15.0f, 0), 8);   // very weak
}

TEST(AdaptiveCR, MidBandStepsUp) {
  // -6 dB SNR = 9 dB margin: between 6 and 12 -> CR 4/6
  EXPECT_EQ(mesh::adaptiveCodingRate(-24, -15.0f, 0), 6);
}

TEST(AdaptiveCR, StrongLinkKeepsDefault) {
  // 0 dB SNR = 15 dB margin >= 12 -> configured default (0)
  EXPECT_EQ(mesh::adaptiveCodingRate(0, -15.0f, 0), 0);
}

TEST(AdaptiveCR, BandEdges) {
  // exactly 6 dB margin (snr4 = -36 at floor -15) -> not below -> mid band
  EXPECT_EQ(mesh::adaptiveCodingRate(-36, -15.0f, 0), 6);
  // 1 step below 6 dB margin -> robust
  EXPECT_EQ(mesh::adaptiveCodingRate(-37, -15.0f, 0), 8);
  // exactly 12 dB margin (snr4 = -12) -> strong
  EXPECT_EQ(mesh::adaptiveCodingRate(-12, -15.0f, 0), 0);
}

TEST(AdaptiveCR, FloorMovesWithSF) {
  // same SNR, SF7 floor is much higher -> same physical SNR is 'stronger' relative to SF10
  EXPECT_EQ(mesh::adaptiveCodingRate(-24, -7.5f, 0), 8);   // margin 1.5 dB
  EXPECT_EQ(mesh::adaptiveCodingRate(0, -7.5f, 0), 6);     // margin 7.5 dB
  EXPECT_EQ(mesh::adaptiveCodingRate(20, -7.5f, 0), 0);    // margin 12.5 dB
}

// ---------------- end-to-end ----------------

namespace {
class TestCRMesh : public BaseChatMesh {
public:
  TestCRMesh(VirtualRadio& r, TestClock& c, mesh::RNG& g, mesh::RTCClock& rtc,
             mesh::PacketManager& m, mesh::MeshTables& t)
      : BaseChatMesh(r, c, g, rtc, m, t) {}

  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  ContactInfo* processAck(const uint8_t*) override { return nullptr; }
  void onContactPathUpdated(const ContactInfo&) override {}
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 5000; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 3000; }
  void onSendTimeout() override {}
};

struct CRNode {
  VirtualRadio radio;
  TestClock clock;
  DeterministicRNG rng;
  FixedRTC rtc;
  SimpleMeshTables tables;
  StaticPoolPacketManager mgr;
  TestCRMesh mesh;

  explicit CRNode(uint8_t id_byte)
      : mgr(16), mesh(radio, clock, rng, rtc, mgr, tables) {
    memset(mesh.self_id.pub_key, id_byte, PUB_KEY_SIZE);
  }
};

// A contact that is a direct neighbour (0 forwarders) with a given measured link
ContactInfo neighborContact(const CRNode& owner, const CRNode& peer, int16_t snr4) {
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = peer.mesh.self_id;
  ci.type = 0;
  uint8_t no_path[1] = {0};
  ci.out_path_len = mesh::Packet::copyPath(ci.out_path, no_path, 0);   // 0-hop direct neighbour
  ci.alt_path_len = OUT_PATH_UNKNOWN;
  ci.path_snr4 = snr4;
  ci.path_snr4_time = owner.rtc.epoch;   // measured "now"
  return ci;
}
}   // namespace

class AdaptiveCRSend : public ::testing::Test {
protected:
  void SetUp() override {
    a = std::make_unique<CRNode>(0xAA);
    b = std::make_unique<CRNode>(0xBB);
    a->mesh.begin();
    b->mesh.begin();
    a->radio.peers.push_back(&b->radio);
    b->radio.peers.push_back(&a->radio);
  }
  std::unique_ptr<CRNode> a, b;

  uint8_t sendAndCr(int16_t snr4) {
    a->mesh.addContact(neighborContact(*a, *b, snr4));
    ContactInfo ci;
    EXPECT_TRUE(a->mesh.getContactByIdx(0, ci));
    uint32_t ack = 0, to = 0;
    a->mesh.sendMessage(ci, 1000, 0, "hello", ack, to);
    for (int i = 0; i < 10 && a->radio.cr_log.empty(); i++) {
      a->mesh.loop();
      a->clock.tick(50);
    }
    EXPECT_FALSE(a->radio.cr_log.empty());
    return a->radio.cr_log.back();
  }
};

TEST_F(AdaptiveCRSend, WeakNeighborSendsRobust) {
  EXPECT_EQ(sendAndCr(-40), 8);   // 5 dB margin at SF10 floor
}

TEST_F(AdaptiveCRSend, MidNeighborStepsUp) {
  EXPECT_EQ(sendAndCr(-24), 6);   // 9 dB margin
}

TEST_F(AdaptiveCRSend, StrongNeighborKeepsDefault) {
  EXPECT_EQ(sendAndCr(0), 0);     // 15 dB margin
}

TEST_F(AdaptiveCRSend, UnmeasuredNeighborKeepsDefault) {
  EXPECT_EQ(sendAndCr(mesh::PATH_SNR_UNKNOWN), 0);
}

TEST_F(AdaptiveCRSend, StaleMeasurementKeepsDefault) {
  ContactInfo ci = neighborContact(*a, *b, -40);
  ci.path_snr4_time -= (mesh::ADAPTIVE_CR_FRESH_SECS + 60);
  a->mesh.addContact(ci);
  ContactInfo got;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, got));
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(got, 1000, 0, "hello", ack, to);
  for (int i = 0; i < 10 && a->radio.cr_log.empty(); i++) {
    a->mesh.loop();
    a->clock.tick(50);
  }
  ASSERT_FALSE(a->radio.cr_log.empty());
  EXPECT_EQ(a->radio.cr_log.back(), 0);
}

TEST_F(AdaptiveCRSend, MultiHopPathNeverAdapts) {
  // 2-forwarder path with a weak recorded bottleneck: our first hop is not the
  // whole story, so no CR override (bottleneck SNR describes the chain, not our link)
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = b->mesh.self_id;
  ci.type = 0;
  uint8_t path[2] = {0xCC, 0xDD};
  ci.out_path_len = mesh::Packet::copyPath(ci.out_path, path, 2);
  ci.alt_path_len = OUT_PATH_UNKNOWN;
  ci.path_snr4 = -40;
  ci.path_snr4_time = a->rtc.epoch;
  a->mesh.addContact(ci);
  ContactInfo got;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, got));
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(got, 1000, 0, "hello", ack, to);
  for (int i = 0; i < 10 && a->radio.cr_log.empty(); i++) {
    a->mesh.loop();
    a->clock.tick(50);
  }
  ASSERT_FALSE(a->radio.cr_log.empty());
  EXPECT_EQ(a->radio.cr_log.back(), 0);
}

TEST_F(AdaptiveCRSend, FloodToSendKeepsDefault) {
  // no path known: message goes by flood, CR untouched (0 = configured default)
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = b->mesh.self_id;
  ci.type = 0;
  ci.out_path_len = OUT_PATH_UNKNOWN;
  ci.alt_path_len = OUT_PATH_UNKNOWN;
  a->mesh.addContact(ci);
  ContactInfo got;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, got));
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(got, 1000, 0, "hello", ack, to);
  for (int i = 0; i < 10 && a->radio.cr_log.empty(); i++) {
    a->mesh.loop();
    a->clock.tick(50);
  }
  ASSERT_FALSE(a->radio.cr_log.empty());
  EXPECT_EQ(a->radio.cr_log.back(), 0);
}
