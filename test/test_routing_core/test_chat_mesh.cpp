// Assertion tests for BaseChatMesh path banking + sender-side route rotation.
#include <gtest/gtest.h>
#include "harness.h"
#include "helpers/BaseChatMesh.h"

using meshcore_test::Node;
using meshcore_test::run;
using meshcore_test::VirtualRadio;
using meshcore_test::TestClock;
using meshcore_test::DeterministicRNG;
using meshcore_test::FixedRTC;

// Minimal BaseChatMesh with recording hooks.
class TestChatMesh : public BaseChatMesh {
public:
  int msgs_recv = 0, contacts_updated = 0;
  std::string last_text;

  TestChatMesh(VirtualRadio& r, TestClock& c, mesh::RNG& g, mesh::RTCClock& rtc,
               mesh::PacketManager& m, mesh::MeshTables& t)
      : BaseChatMesh(r, c, g, rtc, m, t) {}

  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  ContactInfo* processAck(const uint8_t*) override { return nullptr; }
  void onContactPathUpdated(const ContactInfo&) override { contacts_updated++; }
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char* text) override {
    msgs_recv++;
    last_text = text ? text : "";
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 5000; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 3000; }
  void onSendTimeout() override {}
};

struct ChatNode {
  VirtualRadio radio;
  TestClock clock;
  meshcore_test::DeterministicRNG rng;
  meshcore_test::FixedRTC rtc;
  SimpleMeshTables tables;
  StaticPoolPacketManager mgr;
  TestChatMesh mesh;

  explicit ChatNode(uint8_t id_byte)
      : mgr(24), mesh(radio, clock, rng, rtc, mgr, tables) {
    memset(mesh.self_id.pub_key, id_byte, PUB_KEY_SIZE);
  }
};

static void chat_run(ChatNode* n1, ChatNode* n2, int iterations = 30) {
  for (int i = 0; i < iterations; i++) {
    n1->mesh.loop();
    n2->mesh.loop();
    n1->clock.tick(50);
    n2->clock.tick(50);
  }
}

class ChatMeshBanking : public ::testing::Test {
protected:
  void SetUp() override {
    a = std::make_unique<ChatNode>(0xAA);
    b = std::make_unique<ChatNode>(0xBB);
    a->mesh.begin();
    b->mesh.begin();
    a->radio.peers.push_back(&b->radio);
    b->radio.peers.push_back(&a->radio);
  }
  std::unique_ptr<ChatNode> a, b;
};

TEST_F(ChatMeshBanking, AltPathExtraIsBankedIntoContact) {
  // A has B as a contact with a known primary path
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = b->mesh.self_id;
  ci.type = 0;
  ci.out_path_len = 0;      // empty primary (direct neighbor)
  ci.alt_path_len = OUT_PATH_UNKNOWN;
  a->mesh.addContact(ci);

  // B sends a PATH packet carrying the alt-path extra (0x0C)
  uint8_t primary[1] = {0xCC};
  uint8_t alt_extra[2] = {0x01, 0xDD};   // encoded len (count=1,size=1) + hash
  memset(b->mesh.self_id.pub_key, 0xBB, PUB_KEY_SIZE);  // stable id
  uint8_t secret[PUB_KEY_SIZE] = {0};
  mesh::Packet* pkt = b->mesh.createPathReturn(a->mesh.self_id, secret,
                                               primary, 1, PATH_EXTRA_TYPE_ALT_PATH,
                                               alt_extra, sizeof(alt_extra));
  ASSERT_NE(pkt, nullptr);
  b->mesh.sendFlood(pkt, 0u);
  chat_run(a.get(), b.get());

  // A's contact now carries the banked alternate
  ContactInfo got;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, got));
  EXPECT_EQ(got.alt_path_len, 0x01);
  EXPECT_EQ(got.alt_path[0], 0xDD);
}

TEST_F(ChatMeshBanking, RotationUsesAltOnEvenAttempts) {
  // Contact with distinct primary (via 0xCC) and alt (via 0xDD)
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = b->mesh.self_id;
  ci.type = 0;
  uint8_t primary[1] = {0xCC};
  uint8_t alt[1] = {0xDD};
  ci.out_path_len = mesh::Packet::copyPath(ci.out_path, primary, 1);
  ci.alt_path_len = mesh::Packet::copyPath(ci.alt_path, alt, 1);
  a->mesh.addContact(ci);

  ContactInfo send_ci;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, send_ci));
  uint32_t expected_ack = 0, est_timeout = 0;
  // attempt 0 (first send): primary — frame's first forwarder byte is 0xCC
  a->mesh.sendMessage(send_ci, 1000, 0, "first", expected_ack, est_timeout);
  chat_run(a.get(), b.get());
  ASSERT_FALSE(a->radio.last_tx.empty());
  // wire format: [header][path_len=1][path byte][payload...]
  EXPECT_EQ(a->radio.last_tx[2], 0xCC) << "attempt 0 must use the primary route";

  // attempt 2 (a retry): rotates to the alt — first forwarder byte becomes 0xDD
  a->mesh.sendMessage(send_ci, 1000, 2, "retry", expected_ack, est_timeout);
  chat_run(a.get(), b.get());
  EXPECT_EQ(a->radio.last_tx[2], 0xDD) << "attempt 2 must rotate to the alternate";

  // attempt 3 (odd): back to primary
  a->mesh.sendMessage(send_ci, 1000, 3, "retry2", expected_ack, est_timeout);
  chat_run(a.get(), b.get());
  EXPECT_EQ(a->radio.last_tx[2], 0xCC) << "odd attempts stay on primary";
}

