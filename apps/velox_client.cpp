// velox_client -- a reference client for the gateway wire protocol (Spec 007 T5). Connects,
// logs in, then either drives a scenario file (common/scenario.hpp's textual format, reused so
// the same golden scenarios that exercise the engine directly can exercise it through the wire)
// or sends a fixed number of synthetic orders. Prints every received frame -- this doubles as
// the manual-test tool and the fragmentation-test driver.
//
//   velox_client --host=HOST --port=PORT --participant=ID --token=HEX64
//                [--scenario=FILE | --count=N] [--price=P] [--qty=Q]

#include <asio.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "common/scenario.hpp"
#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"
#include "protocol/messages.hpp"

using namespace velox;
using asio::ip::tcp;

namespace {

struct Args {
    std::string host = "127.0.0.1";
    unsigned short port = 9001;
    ParticipantId participant = 1;
    std::string tokenHex;
    std::string scenario;
    long count = 0;
    Price price = 100 * kPriceScale;
    Quantity qty = 10;
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
        if (takeArg(arg, "--host=", tmp))
            a.host = tmp;
        else if (takeArg(arg, "--port=", tmp))
            a.port = static_cast<unsigned short>(std::stoi(tmp));
        else if (takeArg(arg, "--participant=", tmp))
            a.participant = std::stoll(tmp);
        else if (takeArg(arg, "--token=", tmp))
            a.tokenHex = tmp;
        else if (takeArg(arg, "--scenario=", tmp))
            a.scenario = tmp;
        else if (takeArg(arg, "--count=", tmp))
            a.count = std::stol(tmp);
        else if (takeArg(arg, "--price=", tmp))
            a.price = std::stoll(tmp);
        else if (takeArg(arg, "--qty=", tmp))
            a.qty = std::stoll(tmp);
    }
    return a;
}

void hexToToken(const std::string& hex, unsigned char out[32]) {
    for (int i = 0; i < 32; ++i) {
        out[i] = hex.size() == 64
                     ? static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16))
                     : 0;
    }
}

const char* execTypeName(protocol::ExecType t) {
    switch (t) {
        case protocol::ExecType::NewAck:
            return "NEW_ACK";
        case protocol::ExecType::PartialFill:
            return "PARTIAL_FILL";
        case protocol::ExecType::Fill:
            return "FILL";
        case protocol::ExecType::Cancelled:
            return "CANCELLED";
        case protocol::ExecType::Replaced:
            return "REPLACED";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    asio::io_context io;
    tcp::socket socket(io);
    asio::connect(socket, tcp::resolver(io).resolve(args.host, std::to_string(args.port)));

    protocol::LoginMsg login{};
    login.participantId = args.participant;
    hexToToken(args.tokenHex, login.token);
    login.clientSeqNum = 1;
    std::byte buf[128];
    std::size_t n = protocol::encodeLogin(login, buf);
    asio::write(socket, asio::buffer(buf, n));

    protocol::FrameDecoder decoder(1, 0, 100000LL * kPriceScale);
    std::array<std::byte, 256> readBuf{};
    std::uint64_t clientSeq = 2;

    auto readOne = [&](protocol::DecodedMessage& out) -> bool {
        protocol::RejectReason reason;
        for (;;) {
            const auto r = decoder.next(out, reason);
            if (r == protocol::FrameDecoder::Result::Ok) return true;
            if (r == protocol::FrameDecoder::Result::Invalid) return false;
            std::error_code ec;
            const std::size_t got = socket.read_some(asio::buffer(readBuf), ec);
            if (ec || !decoder.feed(readBuf.data(), got)) return false;
        }
    };

    protocol::DecodedMessage msg;
    if (!readOne(msg) || msg.type != protocol::MessageType::LoginAck) {
        std::cerr << "LOGIN failed\n";
        return 1;
    }
    std::cout << "LOGIN_ACK serverSeq=" << msg.loginAck.serverSeq << "\n";

    auto sendNewOrder = [&](OrderId id, Side side, Price price, Quantity qty) {
        protocol::NewOrderMsg m{};
        m.clientSeqNum = clientSeq++;
        m.orderId = id;
        m.instrumentId = 1;
        m.side = side;
        m.orderType = protocol::WireOrderType::Limit;
        m.price = price;
        m.quantity = qty;
        m.timeInForce = protocol::WireTimeInForce::Day;
        std::byte b[128];
        const std::size_t sz = protocol::encodeNewOrder(m, b);
        asio::write(socket, asio::buffer(b, sz));
    };

    if (!args.scenario.empty()) {
        std::ifstream f(args.scenario);
        std::string line;
        while (std::getline(f, line)) {
            common::ScenarioCommand sc;
            if (!common::parseScenarioLine(line, sc)) continue;
            if (sc.kind == common::ScenarioKind::New || sc.kind == common::ScenarioKind::Market) {
                sendNewOrder(sc.id, sc.side, sc.price, sc.quantity);
            }
        }
    } else {
        for (long i = 0; i < args.count; ++i) {
            sendNewOrder(i + 1, (i % 2 == 0) ? Side::Buy : Side::Sell, args.price, args.qty);
        }
    }

    // Drain reports for a short grace period, then disconnect.
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        const std::size_t got = socket.read_some(asio::buffer(readBuf), ec);
        if (!ec && got > 0) {
            decoder.feed(readBuf.data(), got);
            protocol::DecodedMessage m;
            protocol::RejectReason reason;
            while (decoder.next(m, reason) == protocol::FrameDecoder::Result::Ok) {
                if (m.type == protocol::MessageType::ExecReport) {
                    std::cout << "EXEC_REPORT order=" << m.execReport.orderId
                              << " type=" << execTypeName(m.execReport.execType)
                              << " globalSeq=" << m.execReport.globalSeq << "\n";
                } else if (m.type == protocol::MessageType::Reject) {
                    std::cout << "REJECT order=" << m.reject.orderId
                              << " reason=" << static_cast<int>(m.reject.reason) << "\n";
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    return 0;
}
