// Path-quality frame tail (private dialect v90+) — wire round-trip tests.
#include <gtest/gtest.h>
#include "helpers/PathQualityFrame.h"
#include <string.h>

TEST(PathQualityFrame, TailRoundTripWithAlt) {
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.path_snr4 = -24;                  // -6.0 dB bottleneck
  c.alt_path_len = 0x01;
  c.alt_path[0] = 0xDD;
  uint8_t buf[3];
  ASSERT_EQ(appendPathQualityTail(buf, c), 3);
  int16_t snr4 = 0;
  uint8_t flags = 0xFF;
  parsePathQualityTail(buf, snr4, flags);
  EXPECT_EQ(snr4, -24);
  EXPECT_EQ(flags, PATHQ_FLAG_HAS_ALT);
}

TEST(PathQualityFrame, NoAltClearsFlag) {
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.path_snr4 = 12;
  c.alt_path_len = OUT_PATH_UNKNOWN;
  uint8_t buf[3];
  appendPathQualityTail(buf, c);
  int16_t snr4 = 0;
  uint8_t flags = 0xFF;
  parsePathQualityTail(buf, snr4, flags);
  EXPECT_EQ(snr4, 12);
  EXPECT_EQ(flags, 0);
}

TEST(PathQualityFrame, UnknownSentinelSurvivesTheWire) {
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.path_snr4 = mesh::PATH_SNR_UNKNOWN;
  uint8_t buf[3];
  appendPathQualityTail(buf, c);
  int16_t snr4 = 0;
  uint8_t flags = 0;
  parsePathQualityTail(buf, snr4, flags);
  EXPECT_EQ(snr4, mesh::PATH_SNR_UNKNOWN);
}

TEST(PathQualityFrame, V91TailCarriesAltBytes) {
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.path_snr4 = 8;
  c.alt_path_len = 0x02;               // 2 hops, size 1
  c.alt_path[0] = 0xCC; c.alt_path[1] = 0xDD;
  uint8_t buf[32];
  int n = appendPathQualityTailEx(buf, c, 24);
  EXPECT_EQ(n, 6);                     // 3 base + 1 len + 2 bytes
  int16_t snr4; uint8_t flags;
  parsePathQualityTail(buf, snr4, flags);
  EXPECT_EQ(snr4, 8);
  EXPECT_EQ(flags, PATHQ_FLAG_HAS_ALT);
  EXPECT_EQ(buf[3], 0x02);
  EXPECT_EQ(buf[4], 0xCC);
  EXPECT_EQ(buf[5], 0xDD);
}

TEST(PathQualityFrame, V91BudgetCapOmitsBytesKeepsFlag) {
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.path_snr4 = 8;
  c.alt_path_len = 0x40;               // 2 bytes... encoded: count=0? no: 0x40 = size2,count0
  c.alt_path[0] = 0xEE;
  uint8_t buf[32];
  int n = appendPathQualityTailEx(buf, c, 0);   // zero budget
  EXPECT_EQ(n, 3);                     // flag-only fallback
  EXPECT_EQ(buf[2], PATHQ_FLAG_HAS_ALT);
}
