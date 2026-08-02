#pragma once

// Spec 010 T4: static file serving for index.html/app.js/style.css. No build step (T6) -- these
// are plain files on disk, read fresh on every request (this is a demo binary serving three small
// files to a handful of browser tabs, not a production CDN; caching would be premature).

#ifndef VELOX_VIZ_DEFAULT_ASSETS_DIR
#define VELOX_VIZ_DEFAULT_ASSETS_DIR "viz/web"
#endif

#include <fstream>
#include <sstream>
#include <string>

namespace velox::viz {

class StaticFiles {
 public:
    explicit StaticFiles(std::string assetsDir = VELOX_VIZ_DEFAULT_ASSETS_DIR)
        : assetsDir_(std::move(assetsDir)) {}

    // `urlPath` is the raw HTTP request-target (e.g. "/", "/app.js"). Returns false (404) for
    // anything outside the fixed three-file allowlist below -- this server has no directory
    // listing and no path traversal surface, by construction rather than by sanitization.
    bool load(const std::string& urlPath, std::string& outBody, std::string& outContentType) const {
        std::string file;
        if (urlPath == "/" || urlPath == "/index.html") {
            file = "index.html";
            outContentType = "text/html; charset=utf-8";
        } else if (urlPath == "/app.js") {
            file = "app.js";
            outContentType = "application/javascript; charset=utf-8";
        } else if (urlPath == "/style.css") {
            file = "style.css";
            outContentType = "text/css; charset=utf-8";
        } else {
            return false;
        }

        std::ifstream f(assetsDir_ + "/" + file, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        outBody = ss.str();
        return true;
    }

 private:
    std::string assetsDir_;
};

}  // namespace velox::viz
