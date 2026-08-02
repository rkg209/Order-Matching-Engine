#pragma once

// Spec 013 T1: lifted verbatim out of viz/ws_server.cpp's anonymous namespace so admin/
// http_server.cpp (Spec 013) does not duplicate it. Behavior-identical to the original --
// ws_server.cpp now includes this header and drops its own copies, proven by re-running
// `ctest -L viz`.

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

namespace velox::common {

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits a raw "\r\n"-delimited HTTP header block (request line already stripped) into a
// lower-cased-key map. Not a general-purpose HTTP parser -- just enough to find the handful of
// headers a handshake or a small GET-only route table needs.
inline std::unordered_map<std::string, std::string> parseHeaders(const std::string& block) {
    std::unordered_map<std::string, std::string> headers;
    std::istringstream ss(block);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        headers[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return headers;
}

}  // namespace velox::common
