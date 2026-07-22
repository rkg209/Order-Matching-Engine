#pragma once

// The textual order-flow scenario format shared by the golden replay suite (tests/replay/) and
// `velox_live --mode=live` (apps/velox_live.cpp). Factored out here (Spec 006 T8) rather than
// giving velox_live a second parser, per .claude/plans/006-sequencer-journal-recovery.md.
//
//   NEW     <id> <BUY|SELL> <price> <qty> <pid> [IOC|FOK]
//   MARKET  <id> <BUY|SELL> <qty> <pid>
//   CANCEL  <id>
//   REPLACE <oldId> <newId> <BUY|SELL> <price> <qty> <pid>

#include <filesystem>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include "common/types.hpp"
#include "engine/order_book.hpp"

namespace velox::common {

enum class ScenarioKind { New, Market, Cancel, Replace };

struct ScenarioCommand {
    ScenarioKind kind = ScenarioKind::New;
    OrderId id = 0;     // NEW/MARKET/CANCEL: the order id. REPLACE: the OLD id.
    OrderId newId = 0;  // REPLACE only.
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    ParticipantId participant = 0;
    OrderType type = OrderType::Limit;
};

// Parses one line; returns false (leaving `out` untouched) for a blank line, a '#' comment, or
// an unrecognized verb -- forward-compatible with older/newer scenario files, same as the
// original parser this was extracted from.
inline bool parseScenarioLine(const std::string& line, ScenarioCommand& out) {
    if (line.empty() || line[0] == '#') {
        return false;
    }
    std::istringstream ss(line);
    std::string verb, sideStr;
    ScenarioCommand c{};
    double price = 0;

    ss >> verb;
    if (verb == "NEW") {
        ss >> c.id >> sideStr >> price >> c.quantity >> c.participant;
        c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
        c.price = static_cast<Price>(price * kPriceScale);
        std::string tok;
        if (ss >> tok) {
            if (tok == "IOC") {
                c.type = OrderType::Ioc;
            } else if (tok == "FOK") {
                c.type = OrderType::Fok;
            }
        }
        c.kind = ScenarioKind::New;
    } else if (verb == "MARKET") {
        ss >> c.id >> sideStr >> c.quantity >> c.participant;
        c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
        c.type = OrderType::Market;
        c.kind = ScenarioKind::Market;
    } else if (verb == "CANCEL") {
        ss >> c.id;
        c.kind = ScenarioKind::Cancel;
    } else if (verb == "REPLACE") {
        ss >> c.id >> c.newId >> sideStr >> price >> c.quantity >> c.participant;
        c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
        c.price = static_cast<Price>(price * kPriceScale);
        c.kind = ScenarioKind::Replace;
    } else {
        return false;
    }
    out = c;
    return true;
}

inline std::vector<ScenarioCommand> loadScenarioStream(std::istream& in) {
    std::vector<ScenarioCommand> cmds;
    std::string line;
    while (std::getline(in, line)) {
        ScenarioCommand c;
        if (parseScenarioLine(line, c)) {
            cmds.push_back(c);
        }
    }
    return cmds;
}

inline std::vector<ScenarioCommand> loadScenarioFile(const std::filesystem::path& p) {
    std::ifstream in(p);
    return loadScenarioStream(in);
}

}  // namespace velox::common
