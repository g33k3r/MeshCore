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
  int peer_data = 0; int peer_data_len = 0;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override {
    peer_data++; peer_data_len = (int)len;
    BaseChatMesh::onPeerDataRecv(packet, type, sender_idx, secret, data, len);
  }
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


// ── g33k3r fork: LZW TXT compression over the virtual mesh ─────────────────

static void addBidirectional(ChatNode* from, ChatNode* to, uint16_t caps) {
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.id = to->mesh.self_id;
  ci.type = 0;
  ci.out_path_len = OUT_PATH_UNKNOWN;   // flood send (delivery proven path)
  ci.fork_caps = caps;
  from->mesh.addContact(ci);
}

static std::string lzw_sample_text() {
  std::string s = "maintenance reminder: please schedule compressor service. ";
  s += s;
  s += s;
  return s.substr(0, 150);
}

TEST_F(ChatMeshBanking, LzwCompressedRoundTripToCapablePeer) {
  addBidirectional(a.get(), b.get(), ADV_FEAT1_FORK_LZW);
  addBidirectional(b.get(), a.get(), ADV_FEAT1_FORK_LZW);

  ContactInfo send_ci;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, send_ci));
  std::string text = lzw_sample_text();
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(send_ci, 1000, 0, text.c_str(), ack, to);
  chat_run(a.get(), b.get());

  ASSERT_EQ(b->mesh.msgs_recv, 1) << "message must be delivered via the real decrypt path";
  EXPECT_EQ(b->mesh.last_text, text) << "receiver must see the decompressed original text";
}

TEST_F(ChatMeshBanking, LzwStockPeerStillGetsPlainText) {
  addBidirectional(a.get(), b.get(), 0);   // stock caps: never compress
  addBidirectional(b.get(), a.get(), 0);

  ContactInfo send_ci;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, send_ci));
  std::string text = lzw_sample_text();
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(send_ci, 1000, 0, text.c_str(), ack, to);
  chat_run(a.get(), b.get());

  ASSERT_EQ(b->mesh.msgs_recv, 1);
  EXPECT_EQ(b->mesh.last_text, text);
}

TEST_F(ChatMeshBanking, LzwCompressedFrameIsShorterOnTheWire) {
  // Same text, two fresh node pairs: capable receiver gets a smaller packet.
  addBidirectional(a.get(), b.get(), ADV_FEAT1_FORK_LZW);
  addBidirectional(b.get(), a.get(), ADV_FEAT1_FORK_LZW);

  ContactInfo send_ci;
  ASSERT_TRUE(a->mesh.getContactByIdx(0, send_ci));
  std::string text = lzw_sample_text();
  uint32_t ack = 0, to = 0;
  a->mesh.sendMessage(send_ci, 1000, 0, text.c_str(), ack, to);
  chat_run(a.get(), b.get(), 10);
  ASSERT_FALSE(a->radio.last_tx.empty());
  size_t compressed_wire_len = a->radio.last_tx.size();

  // fresh stock pair
  auto c = std::make_unique<ChatNode>(0xCC);
  auto d = std::make_unique<ChatNode>(0xDD);
  c->mesh.begin();
  d->mesh.begin();
  c->radio.peers.push_back(&d->radio);
  d->radio.peers.push_back(&c->radio);
  addBidirectional(c.get(), d.get(), 0);
  addBidirectional(d.get(), c.get(), 0);
  ContactInfo stock_ci;
  ASSERT_TRUE(c->mesh.getContactByIdx(0, stock_ci));
  c->mesh.sendMessage(stock_ci, 1000, 0, text.c_str(), ack, to);
  for (int i = 0; i < 10; i++) {
    c->mesh.loop();
    d->mesh.loop();
    c->clock.tick(50);
    d->clock.tick(50);
  }
  ASSERT_FALSE(c->radio.last_tx.empty());
  size_t stock_wire_len = c->radio.last_tx.size();

  EXPECT_LT(compressed_wire_len, stock_wire_len)
      << "compressed wire (" << compressed_wire_len << ") must beat stock ("
      << stock_wire_len << ") for repetitive text";
}

TEST(LzwAdvertCaps, AdvertFeat1RoundTripsThroughBuilderAndParser) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_CHAT, "caps-node");
  builder.setFeat1(ADV_FEAT1_FORK_LZW);
  uint8_t len = builder.encodeTo(app_data);
  AdvertDataParser parser(app_data, len);
  ASSERT_TRUE(parser.isValid());
  EXPECT_EQ(parser.getFeat1(), ADV_FEAT1_FORK_LZW);
  EXPECT_STREQ(parser.getName(), "caps-node");
  // stock builder: no feat bits set
  AdvertDataBuilder stock(ADV_TYPE_CHAT, "stock-node");
  uint8_t slen = stock.encodeTo(app_data);
  AdvertDataParser sparser(app_data, slen);
  ASSERT_TRUE(sparser.isValid());
  EXPECT_EQ(sparser.getFeat1(), 0);
}
