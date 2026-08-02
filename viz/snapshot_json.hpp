#pragma once

// Spec 010 T4: `{"t":"book","seq":...,"bids":[[price,qty,orders],...],"asks":[...],
// "trades":[{"px":...,"qty":...,"side":"B"|"S"},...],"tradeCount":...}` -- prices stay scaled
// int64 on the wire (well under 2^53, so JS's float64 represents them exactly); the browser
// divides by 10,000 for display only. No floating point crosses this boundary.

#include <cstdint>
#include <sstream>
#include <string>

#include "common/types.hpp"
#include "viz/ladder.hpp"

namespace velox::viz {

inline std::string buildBookSnapshotJson(const Ladder& ladder, std::uint64_t seq,
                                         std::size_t topN) {
    std::ostringstream ss;
    ss << "{\"t\":\"book\",\"seq\":" << seq << ",\"bids\":[";
    bool first = true;
    ladder.topBids(topN, [&](Price p, Quantity q, std::uint32_t n) {
        if (!first) ss << ",";
        first = false;
        ss << "[" << p << "," << q << "," << n << "]";
    });
    ss << "],\"asks\":[";
    first = true;
    ladder.topAsks(topN, [&](Price p, Quantity q, std::uint32_t n) {
        if (!first) ss << ",";
        first = false;
        ss << "[" << p << "," << q << "," << n << "]";
    });
    ss << "],\"trades\":[";
    first = true;
    for (const TradePrint& t : ladder.recentTrades()) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"px\":" << t.price << ",\"qty\":" << t.qty << ",\"side\":\""
           << (t.aggressorSide == Side::Buy ? "B" : "S") << "\"}";
    }
    ss << "],\"tradeCount\":" << ladder.tradeCount() << "}";
    return ss.str();
}

}  // namespace velox::viz
