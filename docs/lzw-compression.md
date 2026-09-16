# LZW Message Compression (g33k3r fork)

Text messages are compressed **inside the end-to-end encryption**, so
intermediate nodes (which route ciphertext blindly) are unaware, and stock
nodes never receive bytes they cannot decode.

## Capability discovery

Upstream reserves two FUTURE feature slots in the advert `app_data` format
(`ADV_FEAT1_MASK` + a 16-bit extra field, encode/parse already implemented in
`AdvertDataHelpers`). This fork claims `feat1` as a capability bitfield:

- bit0 (`ADV_FEAT1_FORK_LZW`) — sender can receive LZW-compressed TXT payloads

Our firmwares set the bit via `AdvertDataBuilder::setFeat1()` in
`CommonCLI::buildAdvertData()` and `BaseChatMesh::createSelfAdvert()`. The
bit travels inside the signed, flooded advert, so capability knowledge
propagates over the mesh; stock nodes parse and ignore unknown feat fields
(zero interop risk). `ContactInfo.fork_caps` stores the learned bits
(transient — re-learned from each advert, never persisted).

## Wire format

Sender (`BaseChatMesh::composeMsgPacket`): when the recipient's `fork_caps`
has the LZW bit and `text_len >= 24` (below that, overhead eats the gain),
the plaintext+terminator is LZW-compressed and the encrypted blob becomes:

```
[timestamp 4][flags 1][0x1F marker][lzw_len u8][lzw stream (lzw_len bytes)]
```

The explicit length byte is load-bearing: compressed streams contain NUL
bytes, so the legacy `strlen`-based ACK-hash semantics cannot bound them.
Both sides now hash exactly `5 + wire_text_len` bytes (identical values for
the stock path — `wire_text_len == strlen(text)` there).

Compression is only applied when the compressed form (plus marker+len) is
strictly shorter than the plaintext — otherwise the legacy layout is sent.

Receiver (`BaseChatMesh::onPeerDataRecv`): `data[5] == 0x1F` → decode exactly
`data[6]` stream bytes → text must end in a NUL → deliver. Corruption or
truncation drops the message **without ACK** — the sender retries or surfaces
the failure, which is the correct semantics for an undecodable payload.

`0x1F` is an ASCII control character; it cannot appear as the first byte of
text entered through the app UIs, so the marker is unambiguous.

## Codec (`src/helpers/LzwCodec.h`)

Header-only, embedded-safe:

- 9→12 bit variable-width codes, dictionary of 4096 entries (12 KB static
  tables — no heap, per the upstream "no dynamic allocation" rule)
- CLEAR code on dictionary exhaustion
- **Adversarially safe decode**: every code checked against `next_code`
  (KwKwK case handled), chain-walk cycle guard, every output write bounded
  by the caller's cap — hostile streams fail with `-1`, never out of bounds

## Honest performance

LZW at TXT sizes (≤161 bytes incl. terminator) is dictionary-warmup bound:
typical conversational English compresses ~1.2–1.4×, repetitive text more.
Measured: 161-byte repetitive sample → 114 bytes (29% saved). The airtime
saving is real but modest; **LZSS** (sliding-window match encoding) is the
identified v2 upgrade for better short-text ratios. The strategic win beyond
airtime: the capability plumbing and exact-length wire format are the
foundation for future payload extensions.

## Test coverage

- `test/test_lzw_codec` (native env): round-trips (incl. max-len,
  incompressible, KwKwK, dictionary-overflow/CLEAR), 2 000 hostile streams +
  full truncation sweep, output-cap enforcement
- `test_routing_core` (virtual mesh): compressed round-trip through the REAL
  encrypt/radio/decrypt path, stock-peer legacy delivery, wire-length
  comparison (compressed < stock), advert feat1 round-trip
