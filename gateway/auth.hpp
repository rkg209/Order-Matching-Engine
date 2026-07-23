#pragma once

// AuthHandler: participantId -> 32-byte token, constant-time compare (Spec 007 T2).
//
// An unknown participant and a bad token must be indistinguishable to the client -- both in
// the reject sent back (LOGIN_REJECT carries no detail) and in timing (the compare always
// walks all 32 bytes, whether or not the participant exists, so branching on "found" cannot
// leak existence through response latency).

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "common/types.hpp"

namespace velox::gateway {

class AuthHandler {
 public:
    using Token = std::array<unsigned char, 32>;

    void addCredential(ParticipantId id, const Token& token) { creds_[id] = token; }

    // Credentials file format: one `participantId hex64` pair per line, `#`-prefixed lines and
    // blank lines skipped. Returns false if the file cannot be opened or a line is malformed.
    bool loadFromFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            return false;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream iss(line);
            long long id = 0;
            std::string hex;
            if (!(iss >> id >> hex) || hex.size() != 64) {
                return false;
            }
            Token tok{};
            for (std::size_t i = 0; i < 32; ++i) {
                tok[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
            }
            addCredential(static_cast<ParticipantId>(id), tok);
        }
        return true;
    }

    // Constant-time in the sense that matters here: the byte-compare loop always runs to
    // completion over all 32 bytes regardless of whether `id` is known, so response timing
    // cannot be used to enumerate valid participant ids.
    bool authenticate(ParticipantId id, const unsigned char token[32]) const noexcept {
        static const Token kDummy{};
        auto it = creds_.find(id);
        const unsigned char* expected = (it != creds_.end()) ? it->second.data() : kDummy.data();

        unsigned char diff = 0;
        for (std::size_t i = 0; i < 32; ++i) {
            diff = static_cast<unsigned char>(diff | (expected[i] ^ token[i]));
        }
        return it != creds_.end() && diff == 0;
    }

 private:
    std::unordered_map<ParticipantId, Token> creds_;
};

}  // namespace velox::gateway
