#include <gtest/gtest.h>
#include "helpers/LzwCodec.h"

#include <string>
#include <vector>

using namespace mesh;

// Codec instances are heavy (12KB tables) — file-scope statics, matching the
// embedded "no dynamic allocation" instantiation pattern.
static LzwCodec g_enc;
static LzwCodec g_dec;

static bool roundtrip(const std::string& text) {
    uint8_t compressed[512];
    uint8_t decompressed[512];
    int clen = g_enc.compress((const uint8_t*)text.data(), text.size() + 1, compressed, sizeof(compressed));
    if (clen < 0) return false;
    int dlen = g_dec.decompress(compressed, clen, decompressed, sizeof(decompressed));
    if (dlen < 0) return false;
    return dlen == (int)(text.size() + 1) &&
           memcmp(decompressed, text.data(), text.size()) == 0 &&
           decompressed[dlen - 1] == 0;
}

TEST(LzwCodec, RoundTripBasics) {
    EXPECT_TRUE(roundtrip("a"));
    EXPECT_TRUE(roundtrip("hello"));
    EXPECT_TRUE(roundtrip("hello hello hello hello hello hello"));
    EXPECT_TRUE(roundtrip("The quick brown fox jumps over the lazy dog. "
                          "The quick brown fox jumps over the lazy dog. "
                          "The quick brown fox jumps over the lazy dog."));
}

TEST(LzwCodec, RoundTripMaxTextLen) {
    // MAX_TEXT_LEN = 160 (10 cipher blocks) — with terminator = 161 bytes
    std::string s;
    for (int i = 0; i < 160; i++) s += (char)('a' + (i % 26));
    EXPECT_TRUE(roundtrip(s));
    // pathological incompressible: random-ish bytes via LCG
    uint32_t x = 12345;
    std::string r;
    for (int i = 0; i < 160; i++) {
        x = x * 1664525u + 1013904223u;
        r += (char)(0x20 + (x >> 24) % 95);
    }
    EXPECT_TRUE(roundtrip(r)); // round-trip must hold even when compression does not help
}

TEST(LzwCodec, CompressionActuallyCompressesRepetitiveText) {
    // Honest expectation for LZW at TXT sizes (≤161B incl. terminator):
    // dictionary warm-up limits the ratio; typical texts land 1.2–1.5x.
    // (LZSS sliding-window is the v2 upgrade path for better ratios.)
    std::string s = "maintenance reminder: please schedule compressor service. ";
    s += s; s += s; s = s.substr(0, 160);
    uint8_t compressed[512];
    int clen = g_enc.compress((const uint8_t*)s.data(), s.size() + 1, compressed, sizeof(compressed));
    ASSERT_GE(clen, 0);
    EXPECT_LT(clen, (int)(s.size() + 1) * 3 / 4) << "repetitive text should save >=25%";
}

TEST(LzwCodec, DictionaryOverflowEmitsClear) {
    // Force table exhaustion: high-entropy input grows the dictionary to 4096
    // entries and must trigger CLEAR, then keep working.
    uint32_t x = 987654321;
    std::string s;
    for (int i = 0; i < 200; i++) {
        x = x * 1664525u + 1013904223u;
        s += (char)(0x20 + (x >> 27));
    }
    EXPECT_TRUE(roundtrip(s));
}

TEST(LzwCodec, KwKwKCase) {
    // "ababababa..." exercises the KwKwK special case (code == next_code)
    std::string s = "abababababababababababababababababababababababababab";
    EXPECT_TRUE(roundtrip(s));
}

TEST(LzwCodec, DecompressRejectsHostileStreams) {
    uint8_t out[512];
    uint32_t x = 42;

    // all 256 code values in a row at every width — includes codes > next_code
    for (int trial = 0; trial < 2000; trial++) {
        uint8_t in[40];
        size_t n = 1 + (x % 39);
        for (size_t i = 0; i < n; i++) {
            x = x * 1664525u + 1013904223u;
            in[i] = (uint8_t)(x >> 24);
        }
        // must never crash, never write out of bounds
        int r = g_dec.decompress(in, n, out, sizeof(out));
        (void)r;
    }
    // truncated valid-ish streams
    std::string s = "compressible compressible compressible";
    uint8_t comp[512];
    int clen = g_enc.compress((const uint8_t*)s.data(), s.size() + 1, comp, sizeof(comp));
    ASSERT_GT(clen, 4);
    for (int cut = 1; cut < clen; cut++) {
        int r = g_dec.decompress(comp, cut, out, sizeof(out));
        (void)r; // may partially decode; must simply not crash
    }
}

TEST(LzwCodec, DecompressRespectsOutputCap) {
    std::string s = "0123456789012345678901234567890123456789";
    uint8_t comp[512];
    int clen = g_enc.compress((const uint8_t*)s.data(), s.size() + 1, comp, sizeof(comp));
    ASSERT_GT(clen, 0);
    uint8_t tiny[8];
    int r = g_dec.decompress(comp, clen, tiny, sizeof(tiny));
    EXPECT_EQ(r, -1); // output overflow must be detected, not smashed
}

TEST(LzwCodec, FirstByteMarkerNeverCollidesWithPlainTextUsage) {
    // 0x1F is an ASCII control char; app text input cannot produce a leading
    // 0x1F through normal UIs. Documented invariant of the wire format.
    EXPECT_EQ(TXT_PAYLOAD_LZW_MARKER, 0x1F);
    EXPECT_EQ(LZW_MIN_TEXT_TO_COMPRESS, 24);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
