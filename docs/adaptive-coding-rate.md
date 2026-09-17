# Link-Adaptive Coding Rate

*Fork feature — roadmap item "Core: dynamic CR (Coding Rate) for weak vs strong hops", scoped to what the fork can prove safe today: zero-hop DIRECT sends over freshly measured links.*

## The idea

LoRa's coding rate (CR) trades payload robustness for airtime. CR 4/5 is the
usual default; CR 4/8 adds ~2.5 dB of decode margin for ~60% more payload
airtime. Two properties make per-packet CR selection free of protocol cost:

1. **CR rides the explicit PHY header of every packet.** Receivers decode
   whatever CR a packet declares — stock MeshCore nodes, flashers at
   meshcore.io, and future upstream builds all decode a CR 4/8 packet from a
   CR 4/5-configured node with no changes. Preamble/CAD detection is
   CR-independent.
2. **The stack's RX-side timing is already worst-cased.**
   `RadioLibWrapper::calcMaxPacketMillis()` rescales payload windows to the
   highest CR, so a neighbour suddenly transmitting CR 4/8 cannot out-wait the
   receiving node's header-valid windows.

## What shipped

| Piece | Where |
|---|---|
| Policy (pure) | `src/helpers/CodingRatePolicy.h` |
| Per-packet hint | `Packet::tx_cr` (RAM-only, zeroed at allocation, never serialized) |
| TX application | `Dispatcher::checkSend()` → `Radio::setCodingRate()` right before `startSendRaw()` |
| Chip support | `applyCodingRate()` on all RadioLib wrappers (SX1262/68, SX1276, LLCC68, LR1110, LR2021, STM32WLx) |
| Link state | `ContactInfo.path_snr4` + new `path_snr4_time` stamp |
| Send-path wiring | `BaseChatMesh::sendDirectContact()` |

### Policy bands

Margin = measured link SNR − demod floor of the current SF (the Semtech-derived
`snr_threshold` table already used for packet scoring).

- **< 6 dB margin → CR 4/8** (robust: rescue marginal links)
- **6–12 dB → CR 4/6** (half-step)
- **≥ 12 dB → configured default** (no override at all)

Stateless by design: CR is a per-packet PHY property, so band-edge flapping is
harmless, and airtime math stays exact because `getTimeOnAir()` reads the CR
actually configured at send time.

### Qualification gates (all must hold)

A send uses an adaptive CR only when:

- it is a **DIRECT send to the contact's stored `out_path`** (not the
  banked alternate — alt-path sends keep plain `sendDirect`), and
- the stored path has **zero forwarders** (`out_path_len & 63 == 0`): our
  radio link is the entire route, so the measurement describes exactly the
  link we're about to use, and
- `path_snr4` is set and **fresher than 10 minutes**.

Anything else — floods, multi-hop directs, unmeasured or stale links — sends
at the configured default. Degradation is exactly yesterday's behaviour.

### Measurement freshness

`path_snr4` for a direct neighbour refreshes at three receipts
(`path_snr4_time` stamps each):

- a PEER_PATH exchange (pre-existing behaviour),
- a **zero-forwarder advert** from that neighbour, and
- a **zero-forwarder plain/signed message** from that neighbour.

On an active chat link the measurement is seconds old; on a silent link it
ages out to the configured default within 10 minutes — the conservative
direction.

### Default handling

`setParams()` records the configured CR per chip wrapper; `setCodingRate(0)`
restores it. `checkSend()` calls `setCodingRate(outbound->tx_cr)` for **every**
transmit (0 for packets without a hint), and the wrapper only touches the
modem when the value actually changes — so after a CR 4/8 rescue send, the
very next flood flies at the configured default again. Duty-cycle and
airtime-budget math read `getTimeOnAir()`, which reflects the applied CR —
worst-case after a robust send, never optimistic.

## Why multi-hop is out of scope (for now)

A 2-hop direct's `path_snr4` is the **bottleneck of the chain**, not our
first hop. Raising our own CR because a *far* hop is weak penalises a strong
local link and helps nobody; lowering it never happens (default is already
fast). Doing this right needs per-neighbour (not per-contact) measurements of
first hops — the natural v2, alongside SF adaptation (which needs
airtime-slot coordination and is *not* PHY-transparent the way CR is).

## Tests

`test/test_routing_core/test_adaptive_cr.cpp` — policy band/edge/floor tests,
plus end-to-end sends through the real Dispatcher TX path asserting the CR the
radio was asked for: weak → 8, mid → 6, strong → 0, unmeasured/stale → 0,
multi-hop → 0, flood → 0.
