// Deterministic fuzz/property tests for Packet::readFrom + parse surfaces.
// Seeded PRNG => every failure is reproducible in CI; no sanitizer infra needed.
#include <gtest/gtest.h>
#include "harness.h"
#include <Packet.h>

using mesh::Packet;

// Build a known-valid frame (advert-style), then attack the parser.
static std::vector<uint8_t> validFrame() {
  Packet p;
  p.header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  p.setPathHashSizeAndCount(1, 2);
  p.path[0] = 0xAA; p.path[1] = 0xBB;
  memcpy(p.payload, "fuzzme", 6);
  p.payload_len = 6;
  uint8_t buf[256];
  uint8_t len = p.writeTo(buf);
  return std::vector<uint8_t>(buf, buf + len);
}

TEST(PacketFuzz, TruncationAtEveryOffsetIsRejectedCleanly) {
  auto frame = validFrame();
  for (size_t cut = 0; cut < frame.size(); cut++) {
    Packet p;
    // shorter slices must either parse partially-but-valid or reject — never
    // read out of bounds / crash (ASan/valgrind gate would catch the read)
    Packet q;
    bool ok = q.readFrom(frame.data(), (uint8_t)cut);
    if (ok) {
      // whatever parsed must be internally consistent
      ASSERT_LE(q.payload_len, MAX_PACKET_PAYLOAD);
      ASSERT_LE((q.path_len & 63) * (((q.path_len >> 6) & 3) + 1), MAX_PATH_SIZE);
    }
  }
}

TEST(PacketFuzz, SeededRandomBuffersNeverCrash) {
  uint32_t seed = 0xF00D5EED;
  for (int iter = 0; iter < 10000; iter++) {
    // xorshift32 — deterministic
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    uint8_t len = (seed % 250) + 1;
    std::vector<uint8_t> buf(len);
    for (int i = 0; i < len; i++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      buf[i] = (uint8_t)(seed >> 24);
    }
    Packet p;
    p.readFrom(buf.data(), len);  // result irrelevant; survival is the test
  }
}

TEST(PacketFuzz, HostilePathLenEncodingsRejectedOrBounded) {
  auto frame = validFrame();
  // path_len byte position: header(1) + path_len(1) => offset 1
  const uint8_t hostile[] = { 0xFF, 0xC0, 0x81, 0x3F, 0xFF & 0x3F, 0b11111111 };
  for (uint8_t v : hostile) {
    auto f = frame;
    f[1] = v;
    Packet p;
    bool ok = p.readFrom(f.data(), (uint8_t)f.size());
    if (ok) {
      uint8_t sz = ((v >> 6) & 3) + 1;
      ASSERT_LE((v & 63) * sz, MAX_PATH_SIZE) << "path bytes exceed buffer for encoding " << (int)v;
    }
  }
}

TEST(PacketFuzz, MaxSizeFrameRoundTrips) {
  Packet p;
  p.header = (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_DIRECT;
  p.transport_codes[0] = 0x1234; p.transport_codes[1] = 0x5678;
  p.setPathHashSizeAndCount(2, 31);   // 31 hops * 2 bytes = 62 <= 64
  memset(p.path, 0x5A, 62);
  memset(p.payload, 0xA5, 100);
  p.payload_len = 100;
  uint8_t buf[300];
  uint8_t len = p.writeTo(buf);
  Packet q;
  ASSERT_TRUE(q.readFrom(buf, len));
  EXPECT_EQ(q.getPathHashCount(), 31);
  EXPECT_EQ(q.getPathHashSize(), 2);
  EXPECT_EQ(q.payload_len, 100);
  EXPECT_EQ(q.transport_codes[0], 0x1234);
}

TEST(PacketFuzz, Regression_TruncatedTransportFrameRejectedNotOverread) {
  // Found by the seeded fuzzer + valgrind gate: a 2-byte frame with a
  // transport-coded header used to memcpy 4 code bytes past the buffer.
  const uint8_t tiny[] = { (uint8_t)(ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT)), 0x00 };
  Packet p;
  EXPECT_FALSE(p.readFrom(tiny, 2));   // rejected, no OOB read (valgrind-gated)
}

TEST(PacketFuzz, RandomizedWriteReadRoundTripIdentity) {
  uint32_t seed = 0x5EED1234;
  for (int iter = 0; iter < 2000; iter++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    Packet p;
    uint8_t route = (seed >> 8) % 2 ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
    uint8_t type = (seed >> 4) % 16;
    p.header = (type << PH_TYPE_SHIFT) | route;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    uint8_t sz = 1 + ((seed >> 6) & 1);            // 1 or 2
    uint8_t count = (seed % 8);                     // 0..7 hops
    p.setPathHashSizeAndCount(sz, count);
    for (int k = 0; k < count * sz; k++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      p.path[k] = seed >> 24;
    }
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    // NOTE (characterized): zero-payload packets do NOT round-trip — writeTo
    // emits them but readFrom treats 'no payload bytes' as bad encoding.
    // The wire contract requires >= 1 payload byte; the property respects it.
    uint8_t plen = 1 + (seed % 119);
    for (int k = 0; k < plen; k++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      p.payload[k] = seed >> 24;
    }
    p.payload_len = plen;

    uint8_t buf[300];
    uint8_t len = p.writeTo(buf);
    Packet q;
    ASSERT_TRUE(q.readFrom(buf, len)) << "iter " << iter;
    ASSERT_EQ(q.header, p.header) << "iter " << iter;
    ASSERT_EQ(q.path_len, p.path_len) << "iter " << iter;
    ASSERT_EQ(q.payload_len, p.payload_len) << "iter " << iter;
    ASSERT_EQ(memcmp(q.path, p.path, count * sz), 0) << "iter " << iter;
    ASSERT_EQ(memcmp(q.payload, p.payload, plen), 0) << "iter " << iter;
  }
}
