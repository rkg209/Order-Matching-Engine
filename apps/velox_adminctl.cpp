// velox_adminctl -- the standalone REST admin daemon (Spec 013, revived at narrow read-only
// scope; constitution.md v1.2). Observes the journal/snapshot directories a velox_gateway process
// (or velox_live, for the pre-Spec-011 flat layout) writes, off disk, O_RDONLY only. Never links
// velox_gateway, never touches a live OrderBook -- see admin/API.md's "source":"disk" caveat.
//
//   velox_adminctl --journal=DIR --port=PORT [--bind=127.0.0.1] [--revoked=FILE]
//
// The secret comes from VELOX_ADMIN_JWT_SECRET (env, never a flag -- flags are visible in `ps`).
// There is no /auth/* route: scripts/issue-admin-token.sh mints tokens offline, and revocation is
// this process re-reading --revoked=FILE (one jti per line) on startup and on SIGHUP.

#include <asio.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_set>

#include "admin/http_server.hpp"
#include "admin/routes.hpp"
#include "admin/store.hpp"

using namespace velox;
using namespace velox::admin;

namespace {

struct Args {
    std::string journalRoot;
    unsigned short port = 8081;
    std::string bindAddr = "127.0.0.1";
    std::string revokedFile;
};

bool takeArg(const std::string& arg, const std::string& key, std::string& out) {
    if (arg.rfind(key, 0) != 0) {
        return false;
    }
    out = arg.substr(key.size());
    return true;
}

Args parseArgs(int argc, char** argv) {
    Args a;
    std::string tmp;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (takeArg(arg, "--journal=", tmp)) {
            a.journalRoot = tmp;
        } else if (takeArg(arg, "--port=", tmp)) {
            a.port = static_cast<unsigned short>(std::stoi(tmp));
        } else if (takeArg(arg, "--bind=", tmp)) {
            a.bindAddr = tmp;
        } else if (takeArg(arg, "--revoked=", tmp)) {
            a.revokedFile = tmp;
        }
    }
    return a;
}

std::unordered_set<std::string> loadRevoked(const std::string& path) {
    std::unordered_set<std::string> out;
    if (path.empty()) return out;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "ADMIN warning: --revoked file not readable: " << path << "\n";
        return out;
    }
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) out.insert(line);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.journalRoot.empty()) {
        std::cerr << "usage: velox_adminctl --journal=DIR --port=PORT [--bind=127.0.0.1] "
                     "[--revoked=FILE]\n";
        return 2;
    }

    const char* secretEnv = std::getenv("VELOX_ADMIN_JWT_SECRET");
    if (secretEnv == nullptr || secretEnv[0] == '\0') {
        std::cerr << "velox_adminctl: VELOX_ADMIN_JWT_SECRET is not set\n";
        return 2;
    }
    const std::string jwtSecret = secretEnv;

    AdminStore store(std::filesystem::path(args.journalRoot));
    std::unordered_set<std::string> revoked = loadRevoked(args.revokedFile);

    asio::io_context io;

    // SIGHUP reloads the revocation list in place. Single-threaded io_context (the entire daemon
    // runs its accept/read/write/route-dispatch chain on this one thread), so mutating `revoked`
    // here and reading it inside a route handler (admin/routes.cpp) never races.
    asio::signal_set sighup(io, SIGHUP);
    std::function<void(std::error_code, int)> onSighup = [&](std::error_code ec, int) {
        if (ec) return;
        revoked = loadRevoked(args.revokedFile);
        std::cerr << "ADMIN revocation list reloaded (" << revoked.size() << " entries)\n";
        sighup.async_wait(onSighup);
    };
    sighup.async_wait(onSighup);

    HttpServer server(io);
    registerAdminRoutes(server, store, jwtSecret, revoked);
    server.listen(args.port, args.bindAddr);

    std::cerr << "ADMIN listening bind=" << args.bindAddr << " port=" << server.localPort()
              << " journal=" << args.journalRoot << " revoked_count=" << revoked.size() << "\n";

    io.run();
    return 0;
}
