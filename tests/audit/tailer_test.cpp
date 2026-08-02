// Spec 012 T7.1: JournalTailer against a LIVE writer -- the crux of T1. No Postgres needed.
//
// Three properties, each proven directly rather than merely asserted:
//   1. Every record is seen exactly once, in order, across a segment roll.
//   2. A torn in-flight write yields NotYet and is delivered once completed -- not dropped, not
//      treated as corruption.
//   3. A checkpoint resume skips exactly the already-seen prefix, never re-delivering it and
//      never skipping past it.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "sequencer/journal_tailer.hpp"
#include "sequencer/journal_writer.hpp"

using namespace velox;
using namespace velox::sequencer;

namespace {

namespace fs = std::filesystem;

ipc::Command makeCommand(OrderId id) {
    ipc::Command cmd{};
    cmd.id = id;
    cmd.price = 100 * kPriceScale;
    cmd.quantity = 10;
    cmd.participant = 1;
    cmd.kind = ipc::CommandKind::New;
    cmd.side = Side::Buy;
    cmd.type = OrderType::Limit;
    return cmd;
}

fs::path freshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() /
                         ("velox_tailer_test_" + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    return dir;
}

}  // namespace

TEST(JournalTailer, SeesEveryRecordExactlyOnceAcrossSegmentRoll) {
    const fs::path dir = freshDir("roll");
    constexpr int kNumRecords = 200;
    // Small enough that kNumRecords worth of kRecordSize-sized records force several rolls.
    const std::size_t rollBytes = SegmentHeader::kSize + 10 * kRecordSize;

    JournalWriter writer(dir, rollBytes);
    JournalTailer tailer(dir, Checkpoint{});

    std::vector<Seq> seen;
    std::thread writerThread([&] {
        for (int i = 1; i <= kNumRecords; ++i) {
            ASSERT_TRUE(writer.append(i, ipc::CommandKind::New, makeCommand(i)));
        }
    });

    int consecutiveNotYet = 0;
    while (static_cast<int>(seen.size()) < kNumRecords) {
        const TailResult r = tailer.next();
        if (r.status == TailStatus::Ok) {
            seen.push_back(r.globalSeq);
            consecutiveNotYet = 0;
        } else {
            ASSERT_EQ(r.status, TailStatus::NotYet);
            ASSERT_LT(++consecutiveNotYet, 1'000'000) << "tailer appears stuck";
        }
    }
    writerThread.join();

    ASSERT_EQ(static_cast<int>(seen.size()), kNumRecords);
    for (int i = 0; i < kNumRecords; ++i) {
        EXPECT_EQ(seen[i], i + 1) << "out of order or duplicated at index " << i;
    }

    fs::remove_all(dir);
}

TEST(JournalTailer, TornInFlightRecordIsNotYetThenDeliveredOnceComplete) {
    const fs::path dir = freshDir("torn");
    JournalWriter writer(dir);

    ASSERT_TRUE(writer.append(1, ipc::CommandKind::New, makeCommand(1)));

    JournalTailer tailer(dir, Checkpoint{});
    ASSERT_EQ(tailer.next().status, TailStatus::Ok);  // consumes record 1

    ASSERT_TRUE(writer.append(2, ipc::CommandKind::New, makeCommand(2)));

    // Simulate a torn tail: cut the last few bytes off record 2 (as if the writer's fsync had
    // not yet landed them), then restore them later to simulate the write completing.
    const fs::path segPath = writer.currentSegmentPath();
    const auto fullSize = fs::file_size(segPath);
    constexpr std::size_t kCut = 5;
    ASSERT_GT(fullSize, kCut);

    unsigned char tail[kCut];
    {
        int fd = ::open(segPath.c_str(), O_RDONLY);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::pread(fd, tail, kCut, static_cast<off_t>(fullSize - kCut)),
                  static_cast<ssize_t>(kCut));
        ::close(fd);
    }

    ASSERT_EQ(::truncate(segPath.c_str(), static_cast<off_t>(fullSize - kCut)), 0);

    // Torn tail: NotYet, not Corrupt, and retryable indefinitely.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(tailer.next().status, TailStatus::NotYet);
    }

    // "Complete" the write by restoring the exact bytes that were cut.
    {
        int fd = ::open(segPath.c_str(), O_WRONLY);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::pwrite(fd, tail, kCut, static_cast<off_t>(fullSize - kCut)),
                  static_cast<ssize_t>(kCut));
        ::close(fd);
    }

    const TailResult r = tailer.next();
    ASSERT_EQ(r.status, TailStatus::Ok);
    EXPECT_EQ(r.globalSeq, 2);

    fs::remove_all(dir);
}

TEST(JournalTailer, CheckpointResumeSkipsExactlyTheSeenPrefix) {
    const fs::path dir = freshDir("resume");
    constexpr int kTotal = 50;
    constexpr int kResumeAfter = 20;
    const std::size_t rollBytes = SegmentHeader::kSize + 7 * kRecordSize;  // force a few rolls

    {
        JournalWriter writer(dir, rollBytes);
        for (int i = 1; i <= kTotal; ++i) {
            ASSERT_TRUE(writer.append(i, ipc::CommandKind::New, makeCommand(i)));
        }
    }

    Checkpoint cp{};
    {
        JournalTailer first(dir, Checkpoint{});
        for (int i = 0; i < kResumeAfter; ++i) {
            ASSERT_EQ(first.next().status, TailStatus::Ok);
        }
        cp = first.checkpoint();
    }
    EXPECT_EQ(cp.lastGoodSeq, kResumeAfter);

    JournalTailer resumed(dir, cp);
    for (int i = kResumeAfter + 1; i <= kTotal; ++i) {
        const TailResult r = resumed.next();
        ASSERT_EQ(r.status, TailStatus::Ok);
        EXPECT_EQ(r.globalSeq, i) << "resume delivered the wrong record at position " << i;
    }
    // Nothing left: exactly the prefix was skipped, nothing more.
    EXPECT_EQ(resumed.next().status, TailStatus::NotYet);

    fs::remove_all(dir);
}
