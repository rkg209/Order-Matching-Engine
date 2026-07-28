#pragma once

// The off-hot-path market-data publisher (Spec 008 T6). Drains a MulticastRing<OutboundEvent>'s
// non-gating consumer index, keeps a BookMirror in sync, derives L2 from the mirror's own level
// totals (the engine emits L3 only -- see the plan's "L2 is derived by the publisher" decision),
// and fans wire-encoded frames out to every attached Sink.
//
// Two independent overrun points, handled differently on purpose (plan's Verification section):
//   1. Ring -> publisher. Non-gating (GatingMask). tryPeekChecked() detects a lap and resync()
//      discards the mirror and rebuilds it from a fresh engine snapshot via SnapshotSource,
//      bracketed by SnapshotStart/.../SnapshotEnd on the wire. The engine is never slowed by this.
//   2. Publisher -> subscriber. That is Sink's problem (marketdata::FeedServer, Spec 008 T8) --
//      this class does not know or care how a Sink delivers bytes, only that it can.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "common/types.hpp"
#include "ipc/outbound_event.hpp"
#include "marketdata/book_mirror.hpp"
#include "marketdata/feed_encoder.hpp"
#include "marketdata/feed_messages.hpp"
#include "marketdata/mirror_digest.hpp"
#include "protocol/message_types.hpp"

namespace velox::marketdata {

// Not the hot path (this is the market-data thread) -- virtual dispatch is fine here. One
// implementation is a real TCP fan-out (FeedServer); tests use an in-process Sink that just
// records frames.
class Sink {
 public:
    virtual ~Sink() = default;
    virtual void send(const std::byte* data, std::size_t n) = 0;
};

namespace detail {
template<class Encoder, class Msg>
inline void publishOne(Sink& sink, Encoder encode, const Msg& m) {
    std::byte buf[protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kMaxFrame];
    const std::size_t n = encode(m, buf);
    sink.send(buf, n);
}
}  // namespace detail

// Replays `mirror`'s current state to exactly ONE sink, bracketed by SnapshotStart/SnapshotEnd --
// the same shape Publisher::resync() broadcasts to every sink on a lap, but usable standalone for
// a single newly-connected subscriber (marketdata::FeedServer's snapshot-burst hook, Spec 008 T8).
// `feedSeq` numbering here is local to this one burst and starts at 1; a subscriber resets its
// own gap-tracking at every SnapshotStart, so this never needs to agree with the live stream's
// counter.
inline void emitSnapshotBurst(Sink& sink, protocol::InstrumentId instrumentId,
                              const BookMirror& mirror) {
    std::uint64_t seq = 0;
    detail::publishOne(
        sink, encodeSnapshotStart,
        SnapshotStartMsg{instrumentId, ++seq, static_cast<std::uint64_t>(mirror.restingOrders())});
    mirror.forEachOrder([&](Side side, const MirrorOrder& o) {
        detail::publishOne(sink, encodeL3Order,
                           L3OrderMsg{instrumentId, ++seq, protocol::L3Action::Rested, o.id, side,
                                      o.price, o.quantity, o.remaining, o.participant});
    });
    detail::publishOne(sink, encodeSnapshotEnd,
                       SnapshotEndMsg{instrumentId, ++seq, digestOf(mirror).bodyCrc32});
}

template<class Ring>
class Publisher {
 public:
    // Called only from resync(): rebuild `mirror` from whatever ground truth the caller wants
    // (in practice, the live engine's OrderBook -- see marketdata/snapshot_source.hpp). Left
    // unset in tests that never force a lap.
    using SnapshotSource = std::function<void(BookMirror&)>;

    explicit Publisher(Ring& ring, protocol::InstrumentId instrumentId,
                       std::size_t consumerIdx = 1) noexcept
        : ring_(ring), instrumentId_(instrumentId), consumerIdx_(consumerIdx) {}

    void addSink(Sink* sink) { sinks_.push_back(sink); }
    void setSnapshotSource(SnapshotSource fn) { snapshotSource_ = std::move(fn); }

    const BookMirror& mirror() const noexcept { return mirror_; }
    std::uint64_t feedSeq() const noexcept { return feedSeq_; }
    std::uint64_t lappedCount() const noexcept { return ring_.lappedCount(consumerIdx_); }
    std::uint64_t lag() const noexcept { return ring_.lag(consumerIdx_); }

    // Drains everything currently available. Returns the number of ring events consumed (a
    // Lapped resync counts as one). Call in a loop from the publisher thread; in tests, call it
    // synchronously after driving commands through the matching thread.
    std::size_t pump() {
        std::size_t drained = 0;
        ipc::OutboundEvent ev{};
        for (;;) {
            const auto status = ring_.tryPeekChecked(consumerIdx_, ev);
            if (status == Ring::PeekStatus::Empty) {
                return drained;
            }
            if (status == Ring::PeekStatus::Lapped) {
                resync();
                ++drained;
                continue;
            }
            ring_.consume(consumerIdx_);
            applyAndPublish(ev);
            ++drained;
        }
    }

 private:
    std::uint64_t nextFeedSeq() noexcept { return ++feedSeq_; }

    template<class Encoder, class Msg>
    void publishMsg(Encoder encode, const Msg& m) {
        std::byte buf[protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kMaxFrame];
        const std::size_t n = encode(m, buf);
        for (Sink* sink : sinks_) {
            sink->send(buf, n);
        }
    }

    void resync() {
        mirror_.clear();
        bidTotals_.clear();
        askTotals_.clear();
        if (snapshotSource_) {
            snapshotSource_(mirror_);
        }

        publishMsg(encodeSnapshotStart,
                   SnapshotStartMsg{instrumentId_, nextFeedSeq(),
                                    static_cast<std::uint64_t>(mirror_.restingOrders())});

        mirror_.forEachOrder([&](Side side, const MirrorOrder& o) {
            publishMsg(encodeL3Order,
                       L3OrderMsg{instrumentId_, nextFeedSeq(), protocol::L3Action::Rested, o.id,
                                  side, o.price, o.quantity, o.remaining, o.participant});
        });
        seedLevelTotals();

        const std::uint32_t crc = digestOf(mirror_).bodyCrc32;
        publishMsg(encodeSnapshotEnd, SnapshotEndMsg{instrumentId_, nextFeedSeq(), crc});
    }

    void seedLevelTotals() {
        // Re-derived from the mirror itself (not accumulated alongside the walk) so it agrees
        // with diffLevel()'s own source of truth exactly -- cheap (one extra lookup per resting
        // order, off the hot path) and immune to drift from computing the same total two
        // different ways. Recomputing per-level for every order in it is redundant but harmless.
        mirror_.forEachOrder([&](Side side, const MirrorOrder& o) {
            auto& totals = (side == Side::Buy) ? bidTotals_ : askTotals_;
            totals[o.price] = mirror_.levelTotal(side, o.price);
        });
    }

    void applyAndPublish(const ipc::OutboundEvent& ev) {
        mirror_.apply(ev);
        switch (ev.kind) {
            case ipc::OutboundKind::TradeEvent: {
                const Trade& t = ev.payload.trade;
                publishMsg(encodeTradeTick,
                           TradeTickMsg{instrumentId_, nextFeedSeq(), t.id, t.aggressorId,
                                        t.passiveId, t.price, t.quantity, t.aggressorSide});
                const Side passiveSide = opposite(t.aggressorSide);
                publishMsg(encodeL3Fill,
                           L3FillMsg{instrumentId_, nextFeedSeq(), t.passiveId, t.id, t.price,
                                     t.quantity, mirror_.remainingOf(t.passiveId)});
                diffLevel(passiveSide, t.price);
                return;
            }
            case ipc::OutboundKind::OrderUpdate: {
                const ipc::OrderUpdate& u = ev.payload.orderUpdate;
                protocol::L3Action action;
                switch (ev.action) {
                    case ipc::UpdateAction::Rested:
                        action = protocol::L3Action::Rested;
                        break;
                    case ipc::UpdateAction::Cancelled:
                        action = protocol::L3Action::Cancelled;
                        break;
                    case ipc::UpdateAction::Replaced:
                        action = protocol::L3Action::Replaced;
                        break;
                    case ipc::UpdateAction::Rejected:
                        // Never rested -- nothing for an L3/L2 subscriber to reconstruct.
                        return;
                }
                publishMsg(encodeL3Order,
                           L3OrderMsg{instrumentId_, nextFeedSeq(), action, u.orderId, ev.side,
                                      u.price, u.quantity, u.remaining, u.participant});
                diffLevel(ev.side, u.price);
                return;
            }
            case ipc::OutboundKind::StatusEvent:
                return;  // no L3/L2 information; the exec-report router already covers this
        }
    }

    void diffLevel(Side side, Price price) {
        std::unordered_map<Price, Quantity>& totals = (side == Side::Buy) ? bidTotals_ : askTotals_;
        const Quantity newTotal = mirror_.levelTotal(side, price);
        const auto it = totals.find(price);
        const Quantity oldTotal = (it == totals.end()) ? 0 : it->second;
        if (newTotal == oldTotal) {
            return;
        }
        protocol::L2Action action;
        if (oldTotal == 0) {
            action = protocol::L2Action::Add;
        } else if (newTotal == 0) {
            action = protocol::L2Action::Delete;
        } else {
            action = protocol::L2Action::Modify;
        }
        if (newTotal == 0) {
            totals.erase(price);
        } else {
            totals[price] = newTotal;
        }
        publishMsg(encodeL2Delta, L2DeltaMsg{instrumentId_, nextFeedSeq(), side, action, price,
                                             newTotal, mirror_.levelOrderCount(side, price)});
    }

    Ring& ring_;
    protocol::InstrumentId instrumentId_;
    std::size_t consumerIdx_;

    BookMirror mirror_;
    std::unordered_map<Price, Quantity> bidTotals_;
    std::unordered_map<Price, Quantity> askTotals_;
    std::uint64_t feedSeq_ = 0;

    SnapshotSource snapshotSource_;
    std::vector<Sink*> sinks_;
};

}  // namespace velox::marketdata
