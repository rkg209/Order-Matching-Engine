// velox_viz -- Spec 010: the live visualizer. A strictly read-only downstream consumer of the
// market-data feed (Spec 008) and the latency-stats feed (Spec 009/010's --stats-port), serving a
// browser ladder + latency histogram over WebSocket. See specs/010-live-visualizer/spec.md and
// .claude/plans/010-live-visualizer.md for the full design; this file is T5, wiring T4's pieces
// together into one binary.
//
//   velox_viz --port=8080 --md=HOST:PORT [--stats=HOST:PORT] [--assets=DIR]
//
// One asio::io_context drives everything: the two upstream clients (MdClient/StatsClient, both
// read-only by construction -- viz/read_only_socket.hpp), the HTTP/WS acceptor, and the ~20 Hz
// ladder-broadcast timer. Nothing here ever writes to the market-data or stats sockets, and
// nothing here is on, or can stall, the matching hot path -- this process does not even link
// against engine/ or book/.

#include <asio.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "protocol/messages.hpp"
#include "viz/md_client.hpp"
#include "viz/snapshot_json.hpp"
#include "viz/stats_client.hpp"
#include "viz/ws_server.hpp"

using namespace velox;

namespace {

struct Args {
    unsigned short port = 8080;  // DR-6
    std::string mdHostPort;
    std::string statsHostPort;
    std::string assetsDir = VELOX_VIZ_DEFAULT_ASSETS_DIR;
    std::size_t topN = 12;
    // Spec 011 T5: 0 means "no filter" -- every instrument on the feed is shown, the
    // pre-sharding behavior. A sharded gateway multiplexes N instruments on one md port, so a
    // single-book viz needs to pick one.
    protocol::InstrumentId instrument = 0;
};

bool takeArg(const std::string& arg, const std::string& key, std::string& out) {
    if (arg.rfind(key, 0) != 0) return false;
    out = arg.substr(key.size());
    return true;
}

Args parseArgs(int argc, char** argv) {
    Args a;
    std::string tmp;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (takeArg(arg, "--port=", tmp)) {
            a.port = static_cast<unsigned short>(std::stoi(tmp));
        } else if (takeArg(arg, "--md=", tmp)) {
            a.mdHostPort = tmp;
        } else if (takeArg(arg, "--stats=", tmp)) {
            a.statsHostPort = tmp;
        } else if (takeArg(arg, "--assets=", tmp)) {
            a.assetsDir = tmp;
        } else if (takeArg(arg, "--top-n=", tmp)) {
            a.topN = std::stoull(tmp);
        } else if (takeArg(arg, "--instrument=", tmp)) {
            a.instrument = static_cast<protocol::InstrumentId>(std::stoul(tmp));
        }
    }
    return a;
}

// "host:port" -> {host, port}. Bracket-free -- every use in this repo is loopback/hostname, IPv4.
bool splitHostPort(const std::string& hostPort, std::string& host, std::string& port) {
    const std::size_t colon = hostPort.rfind(':');
    if (colon == std::string::npos) return false;
    host = hostPort.substr(0, colon);
    port = hostPort.substr(colon + 1);
    return !host.empty() && !port.empty();
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.mdHostPort.empty()) {
        std::cerr << "usage: velox_viz --port=8080 --md=HOST:PORT [--stats=HOST:PORT] "
                     "[--assets=DIR] [--instrument=ID]\n";
        return 2;
    }
    std::string mdHost, mdPort;
    if (!splitHostPort(args.mdHostPort, mdHost, mdPort)) {
        std::cerr << "velox_viz: --md must be HOST:PORT, got '" << args.mdHostPort << "'\n";
        return 2;
    }

    asio::io_context io;

    viz::WsServer wsServer(io, args.assetsDir);
    wsServer.listen(args.port);

    viz::MdClient mdClient(io, mdHost, mdPort, args.instrument);
    mdClient.start();

    std::unique_ptr<viz::StatsClient> statsClient;
    if (!args.statsHostPort.empty()) {
        std::string statsHost, statsPort;
        if (!splitHostPort(args.statsHostPort, statsHost, statsPort)) {
            std::cerr << "velox_viz: --stats must be HOST:PORT, got '" << args.statsHostPort
                      << "'\n";
            return 2;
        }
        statsClient = std::make_unique<viz::StatsClient>(io, statsHost, statsPort);
        // Passed through untouched (viz/stats_client.hpp's doc): the browser sees the exact JSON
        // line the loadgen's reader thread computed, one WS message per stats line -- already
        // >= 1 Hz (NFR-31) because that is telemetry::LiveLatencyStats::onComplete()'s own cadence.
        statsClient->setOnLine(
            [&wsServer](const std::string& line) { wsServer.broadcastText(line); });
        statsClient->start();
    }

    // Coalescing (mandatory -- the plan's T4 note): upstream market-data can be 1M events/sec.
    // This timer pushes the CURRENT top-N ladder state at a fixed ~20 Hz, never one WS message
    // per feed event.
    asio::steady_timer ladderTimer(io);
    std::function<void()> tick = [&] {
        ladderTimer.expires_after(std::chrono::milliseconds(50));
        ladderTimer.async_wait([&](std::error_code ec) {
            if (ec) return;
            if (wsServer.sessionCount() > 0) {
                wsServer.broadcastText(
                    buildBookSnapshotJson(mdClient.ladder(), mdClient.lastSeq(), args.topN));
            }
            tick();
        });
    };
    tick();

    std::cerr << "VIZ listening port=" << args.port << " md=" << args.mdHostPort;
    if (!args.statsHostPort.empty()) {
        std::cerr << " stats=" << args.statsHostPort;
    }
    std::cerr << " assets=" << args.assetsDir;
    if (args.instrument != 0) {
        std::cerr << " instrument=" << args.instrument;
    }
    std::cerr << "\n";

    io.run();
    return 0;
}
