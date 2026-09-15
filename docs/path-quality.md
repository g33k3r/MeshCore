# Path Quality — Design Notes (g33k3r fork)

Status of the arc (all shipped, all CI-gated):

1. **Metric** — `pathQualityScore(hops, bottleneck_snr4)` in `helpers/RoutingPolicy.h`:
   bottleneck SNR dominates (weakest-link), ~1 dB per hop exchange rate,
   `PATH_SNR_UNKNOWN` degrades to hop-count. Pure functions, fully tested.
2. **Measurement** — `traceBottleneckSnr4()` extracts TRACE per-hop minimum;
   repeater + room-server correlate completed traces (same entry size, reversed
   hash order) into `ClientInfo.path_snr4`. Direct-neighbor paths measured at
   receipt via final-hop SNR. Transient fields — never persisted.
3. **Multi-path learning** — `Mesh::onAlternatePathRecv()` fires when a duplicate
   flood arrives via a different route (packet hash covers payload only, so a
   second route is provably new). Adverts (signed pubkey) and message floods
   (unique src-hash match) both feed `considerClientPath()` promotion.

## v2 (designed, NOT yet implemented): dual-path replies

**Goal:** infrastructure tells a sender BOTH its best and second-best route, so
the sender can prefer a strong primary and retain a fallback.

**Wire format** — PATH packet `extra` (inside the encrypted body):
- `extra_type = 0x0C` (`PATH_EXTRA_ALT_PATH` — 0x0C/0x0D are the only free
  payload-type values; parser masks `& 0x0F`, so the marker survives)
- `extra = [alt_path_len (encoded byte)][alt path hash bytes...]`
- Constraint (from `createPathReturn`): `hash_count*hash_size + extra_len + 5
  <= MAX_COMBINED_PATH` — an alt path of up to ~4 entries fits typical replies.

**Free-riding opportunity:** reciprocal PATH replies (`Mesh.cpp` reciprocal
`createPathReturn(..., 0, NULL, 0)` on flood receipt) currently pad `extra`
with 4 random bytes purely to uniquify the hash — the alt path can occupy that
slot at zero marginal airtime (real data uniquifies equally).

**Receiver:** `BaseChatMesh::onContactPathRecv` — new `else if (extra_type ==
PATH_EXTRA_ALT_PATH && extra_len >= 1)` branch; bank into `ContactInfo`
transient `alt_path/alt_path_len` (sentinel `OUT_PATH_UNKNOWN`; field-by-field
contact persistence in `DataStore.cpp` makes struct growth safe — verified).

**Sender choice (v3):** primary-then-alt send policy in `composeMsgPacket`/
`sendDirect` flows — e.g. alt on ACK timeout or measured-weak primary.

**Init audit required when implementing:** `DataStore::loadContacts` +
contact-add paths must set `alt_path_len = OUT_PATH_UNKNOWN` (zero-init is a
VALID path, not unknown).

**Test plan:** harness proof that a PATH packet carrying the alt extra reaches
`onPeerPathRecv` with extra intact (core pass-through); banking test needs
BaseChatMesh in the native env (mock coverage unverified — check Arduino deps
first); diamond-topology end-to-end once both sides exist.
