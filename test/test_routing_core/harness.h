#pragma once
// Two-node/N-node virtual mesh harness for characterizing MeshCore routing.
// All seams are constructor-injected interfaces (Radio, MillisecondClock, RNG,
// RTCClock, PacketManager, MeshTables) — no hardware required.

#include <Mesh.h>
#include "helpers/SimpleMeshTables.h"
#include "helpers/StaticPoolPacketManager.h"

#include <deque>
#include <vector>
#include <cstring>

namespace meshcore_test {

struct VirtualRadio : public mesh::Radio {
  std::deque<std::vector<uint8_t>> rxQ;
  std::vector<VirtualRadio*> peers;
  float snr = -7.5f;
  float rssi = -95.0f;
  float demod_floor = -15.0f;   // fork: SF10-ish floor for adaptive-CR policy tests
  size_t sent = 0;
  bool drop_next = false;   // test hook: swallow next transmission into the void
  std::vector<uint8_t> last_tx;   // raw frame of the most recent transmission
  std::vector<uint8_t> cr_log;    // fork: record setCodingRate calls (0 = restore configured default)

  void setCodingRate(uint8_t cr) override { cr_log.push_back(cr); }
  float getDemodFloorSnr() const override { return demod_floor; }

  int recvRaw(uint8_t* bytes, int sz) override {
    if (rxQ.empty()) return 0;
    std::vector<uint8_t> p = std::move(rxQ.front());
    rxQ.pop_front();
    int n = (int)p.size();
    if (n > sz) n = sz;
    memcpy(bytes, p.data(), n);
    return n;
  }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    if (drop_next) { drop_next = false; return true; }
    sent++;
    last_tx.assign(bytes, bytes + len);
    for (auto* peer : peers) {
      peer->rxQ.emplace_back(bytes, bytes + len);
    }
    return true;
  }
  uint32_t getEstAirtimeFor(int) override { return 10; }
  float packetScore(float, int) override { return 1.0f; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return true; }
  float getLastRSSI() const override { return rssi; }
  float getLastSNR() const override { return snr; }
};

struct TestClock : public mesh::MillisecondClock {
  unsigned long t = 0;
  unsigned long getMillis() override { return t; }
  void tick(unsigned long ms) { t += ms; }
};

struct DeterministicRNG : public mesh::RNG {
  uint8_t seq = 1;
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) dest[i] = (uint8_t)(seq + i * 7);
  }
};

struct FixedRTC : public mesh::RTCClock {
  uint32_t epoch = 1700000000;
  uint32_t getCurrentTime() override { return epoch; }
  void setCurrentTime(uint32_t t) override { epoch = t; }
};

// A Mesh that records what the routing core delivers, and can be configured.
class TestMesh : public mesh::Mesh {
public:
  bool forward_enabled = true;
  bool known_peer = false;
  uint8_t shared_secret[PUB_KEY_SIZE] = {0};

  int adverts = 0, acks = 0, traces = 0, peer_msgs = 0, control = 0, raw = 0;
  int group_msgs = 0, anon_msgs = 0, alt_paths = 0;
  uint8_t last_alt_path[MAX_PATH_SIZE] = {0};
  uint8_t last_alt_path_len = 0;
  uint32_t last_ack_crc = 0;
  std::vector<uint8_t> last_peer_data;
  std::vector<uint8_t> last_group_data;
  std::vector<uint8_t> last_anon_data;
  bool known_channel = false;
  mesh::GroupChannel channel{};

  TestMesh(VirtualRadio& r, TestClock& c, mesh::RNG& g, mesh::RTCClock& rtc,
           mesh::PacketManager& m, mesh::MeshTables& t)
      : mesh::Mesh(r, c, g, rtc, m, t) {}

  bool allowPacketForward(const mesh::Packet*) override { return forward_enabled; }

  void onAdvertRecv(mesh::Packet*, const mesh::Identity&, uint32_t,
                    const uint8_t*, size_t) override { adverts++; }
  void onAckRecv(mesh::Packet*, uint32_t ack_crc) override { acks++; last_ack_crc = ack_crc; }
  void onTraceRecv(mesh::Packet*, uint32_t, uint32_t, uint8_t,
                   const uint8_t*, const uint8_t*, uint8_t) override { traces++; }
  void onPeerDataRecv(mesh::Packet*, uint8_t, int, const uint8_t*,
                      uint8_t* data, size_t len) override {
    peer_msgs++;
    last_peer_data.assign(data, data + len);
  }
  void onControlDataRecv(mesh::Packet*) override { control++; }
  void onRawDataRecv(mesh::Packet*) override { raw++; }

  int searchPeersByHash(const uint8_t*) override { return known_peer ? 1 : 0; }
  int searchChannelsByHash(const uint8_t*, mesh::GroupChannel channels[], int) override {
    if (!known_channel) return 0;
    channels[0] = channel;
    return 1;
  }
  void onGroupDataRecv(mesh::Packet*, uint8_t, const mesh::GroupChannel&,
                       uint8_t* data, size_t len) override {
    group_msgs++;
    last_group_data.assign(data, data + len);
  }
  void onAnonDataRecv(mesh::Packet*, const uint8_t*, const mesh::Identity&,
                      uint8_t* data, size_t len) override {
    anon_msgs++;
    last_anon_data.assign(data, data + len);
  }
  int path_pkts = 0, alt_extras = 0;
  uint8_t last_path_extra_type = 0;
  std::vector<uint8_t> last_path_extra;
  bool onPeerPathRecv(mesh::Packet*, int, const uint8_t*, uint8_t* path, uint8_t path_len,
                      uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override {
    path_pkts++;
    last_path_extra_type = extra_type;
    const uint8_t* base = extra ? extra : (const uint8_t*)"";
    last_path_extra.assign(base, base + (extra ? extra_len : 0));
    if (extra_type == PATH_EXTRA_TYPE_ALT_PATH) alt_extras++;
    return false;
  }
  void onAlternatePathRecv(mesh::Packet*, const uint8_t* path, uint8_t path_len) override {
    alt_paths++;
    last_alt_path_len = path_len;
    if (path_len > 0 && path != nullptr) {
      memset(last_alt_path, 0, sizeof(last_alt_path));
      memcpy(last_alt_path, path, path_len < MAX_PATH_SIZE ? path_len : MAX_PATH_SIZE);
    }
  }
  void getPeerSharedSecret(uint8_t* dest_secret, int) override {
    memcpy(dest_secret, shared_secret, PUB_KEY_SIZE);
  }
};

// One virtual node: everything wired together, in declaration-init order.
struct Node {
  VirtualRadio radio;
  TestClock clock;
  DeterministicRNG rng;
  FixedRTC rtc;
  SimpleMeshTables tables;
  StaticPoolPacketManager mgr;
  TestMesh mesh;

  explicit Node(uint8_t id_byte)
      : mgr(16), mesh(radio, clock, rng, rtc, mgr, tables) {
    memset(mesh.self_id.pub_key, id_byte, PUB_KEY_SIZE);
  }

  void connect(Node& other) {
    radio.peers.push_back(&other.radio);
    other.radio.peers.push_back(&radio);
  }
};

// Drive the network: every node loops once, then time advances.
inline void step(std::initializer_list<Node*> nodes, unsigned long ms = 50) {
  for (auto* n : nodes) n->mesh.loop();
  for (auto* n : nodes) n->clock.tick(ms);
}

inline void run(std::initializer_list<Node*> nodes, int iterations = 30) {
  for (int i = 0; i < iterations; i++) step(nodes);
}

}  // namespace meshcore_test
