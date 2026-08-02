// Spec 013 T7 / plan "Decision: no mutating route, ever -- enforced structurally": velox_adminctl
// is read-only by construction, not by code-review promise. This greps every admin/ source file
// plus apps/velox_adminctl.cpp for any construct capable of writing to the filesystem it
// observes; a single hit fails the build. Same posture as viz/read_only_socket.hpp making the
// visualizer's read-only-ness a compile-time property (CON-7) and
// tests/structural/hot_path_grep_test.py making the hot-path rules mechanical rather than
// review-dependent.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kForbidden = {
    "O_WRONLY", "O_RDWR", "O_CREAT", "ofstream", "fopen(", "std::filesystem::remove", "rename(",
};

std::vector<fs::path> sourceFiles(const fs::path& root) {
    std::vector<fs::path> out;
    const fs::path adminDir = root / "admin";
    if (fs::exists(adminDir)) {
        for (const auto& e : fs::recursive_directory_iterator(adminDir)) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext == ".hpp" || ext == ".cpp") out.push_back(e.path());
        }
    }
    const fs::path adminctl = root / "apps" / "velox_adminctl.cpp";
    if (fs::exists(adminctl)) out.push_back(adminctl);
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(AdminReadOnly, NoMutatingFilesystemConstructs) {
#ifndef VELOX_SOURCE_ROOT
    GTEST_SKIP() << "VELOX_SOURCE_ROOT not defined";
#else
    const fs::path root(VELOX_SOURCE_ROOT);
    const std::vector<fs::path> files = sourceFiles(root);
    ASSERT_GT(files.size(), 0u) << "expected to find admin/ sources under " << root.string();

    for (const auto& file : files) {
        const std::string content = readFile(file);
        for (const auto& forbidden : kForbidden) {
            EXPECT_EQ(content.find(forbidden), std::string::npos)
                << file.string() << " contains forbidden construct \"" << forbidden
                << "\" -- velox_adminctl must never write to the filesystem it observes";
        }
    }
#endif
}
