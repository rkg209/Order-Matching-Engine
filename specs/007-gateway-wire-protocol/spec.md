# Spec 007 — Binary wire protocol + order gateway

**Status:** 📋 BACKLOG · **Phase:** C — Real exchange surface · **Depends on:** 006

## Scope

A length-prefixed binary protocol over TCP and the gateway that speaks it: decode, validate,
authenticate, publish to the ingress ring, and route execution reports back to the originating
connection.

## Behavior

Message types (fixed-size payloads, all prices scaled `int64`):

| Message | Payload | Fields |
|---|---|---|
| `LOGIN` | 41 B | `participantId`(8) `token`(32) `clientSeqNum`(1)  ⚠️ *see conflict below* |
| `NEW_ORDER` | 39 B | `clientSeqNum`(8) `orderId`(8) `instrumentId`(4) `side`(1) `orderType`(1) `price`(8) `quantity`(8) `timeInForce`(1) |
| `CANCEL` | 20 B | `clientSeqNum`(8) `orderId`(8) `instrumentId`(4) |
| `CANCEL_REPLACE` | 36 B | `clientSeqNum`(8) `orderId`(8) `instrumentId`(4) `newPrice`(8) `newQuantity`(8) |
| `EXEC_REPORT` | 49 B | `orderId`(8) `execType`(1) `execQty`(8) `leavesQty`(8) `tradePrice`(8) `tradeId`(8) `globalSeq`(8) |
| `REJECT` | 17 B | `orderId`(8) `rejectReason`(1) `globalSeq`(8) |
| `HEARTBEAT` | 8 B | every 30 s |

- **Auth before orders** (FR-24). Unauthenticated connections are closed after a timeout (NFR-28).
- **Client sequence numbers** per connection: detect duplicates and gaps (FR-25).
- **Boost.Asio async I/O**, one session per connection. The gateway is the **only component that
  allocates freely** — it is off the hot path, and pretending otherwise would cripple it for no gain.
- **Backpressure:** ring full ⇒ **stop reading the socket** ⇒ TCP flow control (FR-28). Never drop.

## ⚠️ Conflict to resolve

`05-api-design.md` gives `LOGIN` a **1-byte** `clientSeqNum`, while every other message gives it
**8 bytes**. A 1-byte sequence number wraps after 255 messages, which would make gap detection
meaningless. This is almost certainly a typo in the doc. **Resolve it to 8 bytes** unless there is a
reason not to — and record the decision, because a wire protocol is the one thing you cannot quietly
change later.

## Security posture (this is the hostile surface)

The gateway parses **untrusted bytes from the network**. It is the only part of this system an attacker
can reach, and every other component's correctness assumes the gateway did its job.

- **Reject all malformed, truncated, and hostile input** (NFR-26): bad length prefix, unknown message
  type, out-of-range fields, missing fields, absurd quantities, negative prices.
- **No crash. No state corruption. No internal-state leak** in an error message.
- **Length-prefix validation before allocation** — a 4-byte length field claiming 4 GB must be rejected,
  not honored. Trusting an attacker-supplied length is the classic remote DoS, and it is one line to
  get wrong.
- **Fuzz pass: ≥ 10,000 randomized malformed frames, zero crashes, zero corruptions** (NFR-27).

## Definition of Done

- [ ] Protocol round-trip tests: encode → decode → identical struct, for every message type.
- [ ] Partial/fragmented frames across TCP reads are handled — a message split across two `read()`s must
      reassemble. (TCP is a **stream**, not a message protocol. Assuming one read == one message is the
      most common bug in hand-rolled wire protocols, and it will not show up in local testing where
      messages happen to arrive whole.)
- [ ] Malformed and hostile input is rejected with a structured reject; no crash, no corruption.
- [ ] A fuzz pass of ≥10,000 malformed frames finds **no crash** (NFR-27, FR-29).
- [ ] Duplicate and gapped client sequence numbers are detected.
- [ ] Backpressure verified: full ring stops the reader; no order dropped.
- [ ] Execution reports route back to the **originating** connection (FR-27).

## Requirements satisfied

FR-22…FR-30 · NFR-26 (reject hostile input) · NFR-27 (fuzz) · NFR-28 (auth timeout)
