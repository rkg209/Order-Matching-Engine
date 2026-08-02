// velox_loadgen -- Spec 009: the rate-driven load generator.
//
// Drives one of three real paths (--path=engine|durable|wire) against an INTENDED schedule
// (loadgen/schedule.hpp), records both a naive and a coordinated-omission-corrected latency
// histogram per order (loadgen/latency_recorder.hpp), and reports p50/p99/p999 + throughput with
// every honesty guard the methodology skill requires. See .claude/plans/009-benchmark-harness.md
// for the design rationale -- this file is the "Implementation / T3" section of that plan, made
// real.

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/order_book.hpp"
#include "loadgen/demo_feed.hpp"
#include "loadgen/inflight.hpp"
#include "loadgen/latency_recorder.hpp"
#include "loadgen/paths.hpp"
#include "loadgen/schedule.hpp"
#include "loadgen/workload.hpp"
#include "platform/platform.hpp"
#include "sequencer/journal_reader.hpp"

using namespace velox;
using namespace velox::loadgen;

namespace {

struct Args {
    std::string path = "engine";
    std::string workload = "dense";
    std::string rate = "100000";  // "max" => saturation mode
    std::size_t samples = 1'000'000;
    std::size_t warmup = 100'000;
    std::size_t groupCommit = 0;
    std::string jsonOut;
    std::string csvOut;
    long injectStallMs = 0;
    std::size_t injectStallAt = 0;

    // Spec 011 T7: 0 means "not sharded" (the pre-existing single-path behavior below). >=1
    // drives N independent EnginePath-equivalents instead -- see runSharded().
    std::size_t shards = 0;

    // Spec 010 T3: the visualizer demo driver's flags. md-port/stats-port work with any of the
    // three --path= drivers that expose outRing() (today, just `engine` -- see paths.hpp);
    // replay-journal is its own mode entirely (runReplay(), below), always on the engine path.
    unsigned short mdPort = 0;
    unsigned short statsPort = 0;
    std::string replayJournal;
    bool startOnSubscriber = false;
    bool loop = false;
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
        if (takeArg(arg, "--path=", tmp)) {
            a.path = tmp;
        } else if (takeArg(arg, "--workload=", tmp)) {
            a.workload = tmp;
        } else if (takeArg(arg, "--rate=", tmp)) {
            a.rate = tmp;
        } else if (takeArg(arg, "--samples=", tmp)) {
            a.samples = std::stoull(tmp);
        } else if (takeArg(arg, "--warmup=", tmp)) {
            a.warmup = std::stoull(tmp);
        } else if (takeArg(arg, "--group-commit=", tmp)) {
            a.groupCommit = std::stoull(tmp);
        } else if (takeArg(arg, "--json-out=", tmp)) {
            a.jsonOut = tmp;
        } else if (takeArg(arg, "--csv-out=", tmp)) {
            a.csvOut = tmp;
        } else if (takeArg(arg, "--inject-stall-ms=", tmp)) {
            a.injectStallMs = std::stol(tmp);
        } else if (takeArg(arg, "--inject-stall-at=", tmp)) {
            a.injectStallAt = std::stoull(tmp);
        } else if (takeArg(arg, "--shards=", tmp)) {
            a.shards = std::stoull(tmp);
        } else if (takeArg(arg, "--md-port=", tmp)) {
            a.mdPort = static_cast<unsigned short>(std::stoi(tmp));
        } else if (takeArg(arg, "--stats-port=", tmp)) {
            a.statsPort = static_cast<unsigned short>(std::stoi(tmp));
        } else if (takeArg(arg, "--replay-journal=", tmp)) {
            a.replayJournal = tmp;
        } else if (arg == "--start-on-subscriber") {
            a.startOnSubscriber = true;
        } else if (arg == "--loop") {
            a.loop = true;
        }
    }
    return a;
}

// Result of one measured run -- everything the report/JSON/CSV/gate stages need.
struct RunResult {
    std::string scenario;
    std::string path;
    std::string workload;
    bool durable = false;
    bool wire = false;
    std::size_t samples = 0;
    double intendedRate = 0.0;
    double achievedRate = 0.0;
    bool rateSustained = true;
    bool poolExhausted = false;
    long long p50Ns = 0, p99Ns = 0, p999Ns = 0, maxNs = 0;
    bool p999Reported = false;
    long long naiveP999Ns = 0;
    bool pinned = false;
    std::size_t fullSpins = 0;
    std::uint64_t lappedCount = 0;
};

void writeJson(const Args& args, const RunResult& r) {
    if (args.jsonOut.empty()) return;
    std::ofstream f(args.jsonOut);
    f << "{\n";
    f << "  \"scenario\": \"" << r.scenario << "\",\n";
    f << "  \"path\": \"" << r.path << "\",\n";
    f << "  \"workload\": \"" << r.workload << "\",\n";
    f << "  \"durable\": " << (r.durable ? "true" : "false") << ",\n";
    f << "  \"wire\": " << (r.wire ? "true" : "false") << ",\n";
    f << "  \"sample_count\": " << r.samples << ",\n";
    f << "  \"intended_rate\": " << r.intendedRate << ",\n";
    f << "  \"throughput_ops_sec\": " << r.achievedRate << ",\n";
    f << "  \"rate_sustained\": " << (r.rateSustained ? "true" : "false") << ",\n";
    f << "  \"pool_exhausted\": " << (r.poolExhausted ? "true" : "false") << ",\n";
    f << "  \"p50_ns\": " << r.p50Ns << ",\n";
    f << "  \"p99_ns\": " << r.p99Ns << ",\n";
    if (r.p999Reported) {
        f << "  \"p999_ns\": " << r.p999Ns << ",\n";
    }
    f << "  \"max_ns\": " << r.maxNs << ",\n";
    f << "  \"naive_p999_ns\": " << r.naiveP999Ns << ",\n";
    f << "  \"pinned\": " << (r.pinned ? "true" : "false") << ",\n";
    f << "  \"full_spins\": " << r.fullSpins << ",\n";
    f << "  \"lapped_count\": " << r.lappedCount << ",\n";
    f << "  \"core_isolation\": " << (platform::supportsCoreIsolation() ? "true" : "false")
      << ",\n";
    f << "  \"platform\": \"" << platform::platformName() << "\"\n";
    f << "}\n";
}

void writeCsv(const std::string& path, hdr_histogram* corrected, hdr_histogram* naive) {
    if (path.empty()) return;
    std::ofstream f(path);
    f << "percentile,corrected_ns,naive_ns\n";
    for (double p : {50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999}) {
        f << p << "," << hdr_value_at_percentile(corrected, p) << ","
          << hdr_value_at_percentile(naive, p) << "\n";
    }
    f << "100,\"" << hdr_max(corrected) << "\",\"" << hdr_max(naive) << "\"\n";
}

// Drives `path` (any of EnginePath/DurablePath/WirePath) end to end: populate, warm up, measure,
// report. Templated so the same driver logic runs unmodified over all three concrete path types
// (T2's shared shape) rather than needing virtual dispatch on the hot loop.
template<class Path>
RunResult run(Path& path, const Args& args, const std::string& scenarioName) {
    const Workload workload = args.workload == "thin" ? Workload::Thin : Workload::Dense;

    OrderId nextId = 1;
    std::vector<ipc::Command> pop = populationCommands(workload, nextId);
    for (const auto& c : pop) {
        path.sendOne(c);
    }
    path.waitProcessed(pop.size());

    const bool saturation = (args.rate == "max");
    const std::int64_t intervalNs = saturation ? 0 : (1'000'000'000LL / std::stoll(args.rate));

    // Spec 010 T3: the visualizer demo feed. Only EnginePath exposes outRing() (paths.hpp), so
    // this is a no-op -- with a stderr note -- on --path=durable/wire.
    std::unique_ptr<DemoFeed> demoFeed;
    if (args.mdPort != 0 || args.statsPort != 0) {
        if constexpr (requires { path.outRing(); }) {
            demoFeed = std::make_unique<DemoFeed>(path.outRing(), args.mdPort, args.statsPort);
        } else {
            std::fprintf(stderr,
                         "velox_loadgen: --md-port/--stats-port need --path=engine (got %s)\n",
                         Path::kName);
        }
    }
    if (args.startOnSubscriber && demoFeed) {
        while (demoFeed->mdSubscribers() == 0) {
            platform::cpuPause();
        }
    }

    InflightTable inflight;
    LatencyRecorder recorder;
    // Every OrderUpdate the reader observes increments this, population/warmup/measured alike --
    // it exists only to know when the pipeline has fully drained, never to gate histogram
    // content (that split is by id range in recordRange() below, after the reader has stopped).
    std::atomic<std::size_t> completed{0};

    const auto t0 = Clock::now();
    path.startReader(
        [&](OrderId id, std::int64_t completeNs) {
            inflight.recordComplete(id, completeNs);
            completed.fetch_add(1, std::memory_order_relaxed);
            if (demoFeed) {
                demoFeed->onComplete(completeNs - inflight.intendedNs(id),
                                     intervalNs > 0 ? intervalNs : 1);
            }
        },
        t0);

    SteadyStateGenerator gen(workload, thinTopPrice());
    Schedule schedule(t0, intervalNs > 0 ? intervalNs : 1);

    // Saturation mode (--rate=max) has no intended schedule to be coordinatedly-omitted
    // against -- "as fast as possible" IS the intended send time, so intendedNs is set to
    // actualSendNs (never 0) when saturating. Setting it to 0 would make correctedSpan =
    // completeNs (time since t0, unrelated to this order's actual send time) and feed that huge,
    // ever-growing span into hdr_record_corrected_value() with a 1ns interval below -- an
    // O(span/interval) backfill per sample that turns a normal run into an effectively infinite
    // one. CO-correction is meaningless without a target rate to miss; saturation mode's
    // "corrected" histogram is therefore identical to its naive one, which is the honest answer.
    auto sendIndexed = [&](std::size_t i) {
        const OrderId id = nextId++;
        if (!saturation) {
            Schedule::waitUntil(schedule.intendedTime(i));
        }
        const std::int64_t actualSendNs = nsSince(t0);
        const std::int64_t intendedNs = saturation ? actualSendNs : schedule.intendedNs(i);
        inflight.recordSend(id, intendedNs, actualSendNs);
        ipc::Command cmd = gen.next(id);
        path.sendOne(cmd);
    };

    // Warmup: same path, same rate. Its completions are drained same as everything else, but
    // never fed into recordRange() below, so they never reach the histograms.
    for (std::size_t i = 0; i < args.warmup; ++i) {
        sendIndexed(i);
    }

    const OrderId measuredFirstId = nextId;
    const auto measureStart = Clock::now();
    for (std::size_t i = 0; i < args.samples; ++i) {
        if (args.injectStallMs > 0 && i == args.injectStallAt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(args.injectStallMs));
        }
        sendIndexed(args.warmup + i);
    }
    const auto measureSendEnd = Clock::now();

    // Drain: give the reader a generous window to observe every completion (population + warmup
    // + measured) before the histograms are built. Scaled by sample count, not a fixed constant:
    // the durable/wire paths can be fsync- or TCP-bound to a few hundred completions/sec (the
    // honest number, not a bug -- see hardware.md), so a fixed short deadline would truncate a
    // slow-but-healthy run and misreport it as an incomplete one.
    const std::size_t totalTracked = pop.size() + args.warmup + args.samples;
    const auto drainDeadline =
        Clock::now() + std::chrono::seconds(std::max<long long>(60, totalTracked / 50 + 10));
    while (completed.load(std::memory_order_relaxed) < totalTracked &&
           Clock::now() < drainDeadline) {
        platform::cpuPause();
    }

    path.stopReader();

    // Single fast sequential pass over just the measured ids -- NOT interleaved with the reader,
    // so hdr_record_corrected_value()'s per-call cost can never compete with ring-draining speed
    // (see inflight.hpp's file header).
    recordRange(recorder, inflight, measuredFirstId, args.samples, intervalNs > 0 ? intervalNs : 1);

    RunResult r;
    r.scenario = scenarioName;
    r.path = Path::kName;
    r.workload = args.workload;
    r.durable = Path::kDurable;
    r.wire = Path::kWire;
    r.samples = args.samples;
    r.intendedRate = saturation ? 0.0 : (1e9 / static_cast<double>(intervalNs));

    const double secs = std::chrono::duration<double>(measureSendEnd - measureStart).count();
    r.achievedRate = secs > 0.0 ? static_cast<double>(args.samples) / secs : 0.0;
    r.rateSustained = saturation || (r.achievedRate >= 0.95 * r.intendedRate);

    r.poolExhausted = false;
    if constexpr (requires { path.poolExhaustedRejects(); }) {
        r.poolExhausted = path.poolExhaustedRejects() > 0;
    }

    r.p50Ns = LatencyRecorder::percentile(recorder.corrected(), 50.0);
    r.p99Ns = LatencyRecorder::percentile(recorder.corrected(), 99.0);
    r.p999Reported = args.samples >= 1'000'000;
    r.p999Ns = r.p999Reported ? LatencyRecorder::percentile(recorder.corrected(), 99.9) : 0;
    r.maxNs = static_cast<long long>(hdr_max(recorder.corrected()));
    r.naiveP999Ns = LatencyRecorder::percentile(recorder.naive(), 99.9);

    if constexpr (requires { path.pinned(); }) {
        r.pinned = path.pinned();
        r.fullSpins = path.fullSpins();
        r.lappedCount = path.lappedCount();
    }

    std::printf("\n=== velox_loadgen: path=%s workload=%s scenario=%s ===\n", r.path.c_str(),
                r.workload.c_str(), r.scenario.c_str());
    std::printf("  platform:            %s\n", platform::platformName());
    std::printf("  core isolation:      %s\n",
                platform::supportsCoreIsolation() ? "available" : "NOT AVAILABLE");
    if constexpr (requires { path.pinned(); }) {
        std::printf("  matching pinned:     %s\n", r.pinned ? "yes" : "NOT PINNED");
        std::printf("  full spins:          %zu\n", r.fullSpins);
        std::printf("  lapped (consumer 0): %llu\n",
                    static_cast<unsigned long long>(r.lappedCount));
    }
    std::printf("  samples:             %zu (after %zu warmup)\n", r.samples, args.warmup);
    std::printf("  target rate:         %s\n",
                saturation ? "max (saturation)" : (args.rate + " /sec").c_str());
    std::printf("  achieved rate:       %.0f /sec\n", r.achievedRate);
    if (!r.rateSustained) {
        std::printf(
            "  *** RATE NOT SUSTAINED -- this run cannot be compared at the target rate. ***\n");
    }
    if (r.poolExhausted) {
        std::printf("  *** POOL EXHAUSTED -- this run measured the REJECT path. Invalid. ***\n");
    }
    std::printf("\n");
    std::printf("  CORRECTED (report this): p50 %8lld ns   p99 %8lld ns   p999 %s\n", r.p50Ns,
                r.p99Ns,
                r.p999Reported ? (std::to_string(r.p999Ns) + " ns").c_str()
                               : "NOT REPORTED (need >= 1,000,000 samples)");
    std::printf("  NAIVE (what a naive loop would have reported): p999 %8lld ns\n", r.naiveP999Ns);
    std::printf("\n");
    std::printf("  CAVEATS:\n");
    std::printf("    - Reader-thread-observed completion: includes reader wakeup + outbound\n");
    std::printf("      publish, a strict superset of order-to-match for engine/durable.\n");
    if (Path::kWire) {
        std::printf(
            "    - WIRE: first EXEC_REPORT/REJECT for the id. This is the synchronous NEW_ACK\n"
            "      (gateway/session.cpp sendNewAckIfApplicable), sent the instant the sequencer\n"
            "      durably assigns a sequence number -- BEFORE the matching thread necessarily\n"
            "      dispatches the command. So this figure can UNDERSTATE order-to-match for a\n"
            "      resting order; it is sequencer+TCP-round-trip dominated, not gated.\n");
    }
    if (Path::kDurable) {
        std::printf("    - DURABLE: includes fsync (%s). Not gated vs the engine budget.\n",
                    platform::fsyncMechanismName());
    }
    std::printf("======================================================================\n\n");

    writeJson(args, r);
    writeCsv(args.csvOut, recorder.corrected(), recorder.naive());
    return r;
}

// Spec 010 T3: the deterministic-demo mode -- replays a pre-recorded journal (instead of
// SteadyStateGenerator's synthetic workload) at a paced rate, through the same EnginePath the
// live demo uses, so a --md-port/--stats-port subscriber sees the identical book history every
// run (FR-40). Always the engine path: a replayed demo has no reason to pay fsync/TCP cost, and
// journal commands already carry the ids/prices that produced the recorded book, so this is
// purely about REPLAYING them, not about exercising the durable/wire paths a second time.
void runReplay(const Args& args) {
    // Read the whole journal up front -- allocation happens here, in setup, never in the paced
    // send loop below (same "never interleave setup cost with the measured/demo loop" discipline
    // as InflightTable/LatencyRecorder's post-run pass).
    sequencer::JournalReader reader(args.replayJournal);
    std::vector<ipc::Command> commands;
    for (;;) {
        const sequencer::ReadResult r = reader.next();
        if (r.status != sequencer::ReadStatus::Ok) {
            break;  // EndOfJournal (expected), or TruncatedTail/Corrupt (stop at last good record)
        }
        commands.push_back(r.command);
    }
    if (commands.empty()) {
        std::fprintf(stderr, "velox_loadgen: --replay-journal=%s produced no commands\n",
                     args.replayJournal.c_str());
        return;
    }
    std::printf("velox_loadgen: replaying %zu commands from %s\n", commands.size(),
                args.replayJournal.c_str());

    EnginePath path;
    DemoFeed demoFeed(path.outRing(), args.mdPort, args.statsPort);

    // FR-40: a subscriber must see the stream from record 0 every time -- so nothing is sent
    // until at least one is connected, making the md byte stream byte-identical run to run.
    if (args.startOnSubscriber) {
        while (demoFeed.mdSubscribers() == 0) {
            platform::cpuPause();
        }
    }

    const bool saturation = (args.rate == "max");
    const std::int64_t intervalNs = saturation ? 0 : (1'000'000'000LL / std::stoll(args.rate));

    const auto t0 = Clock::now();
    InflightTable inflight;
    path.startReader(
        [&](OrderId id, std::int64_t completeNs) {
            demoFeed.onComplete(completeNs - inflight.intendedNs(id),
                                intervalNs > 0 ? intervalNs : 1);
        },
        t0);

    do {
        Schedule schedule(Clock::now(), intervalNs > 0 ? intervalNs : 1);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            if (!saturation) {
                Schedule::waitUntil(schedule.intendedTime(i));
            }
            const std::int64_t intendedNs = schedule.intendedNs(i);
            const std::int64_t actualSendNs = nsSince(t0);
            // Replace's completion arrives keyed by newId, not the record's own `id` (the OLD
            // id) -- see ipc/command.hpp. Recording under both keys keeps the live latency
            // figure meaningful for Replace without needing a second correlation table.
            inflight.recordSend(static_cast<std::uint64_t>(commands[i].id), intendedNs,
                                actualSendNs);
            if (commands[i].kind == ipc::CommandKind::Replace) {
                inflight.recordSend(static_cast<std::uint64_t>(commands[i].newId), intendedNs,
                                    actualSendNs);
            }
            path.sendOne(commands[i]);
        }
        std::printf("velox_loadgen: replay pass complete (%zu commands)\n", commands.size());
        demoFeed.waitDrained();
    } while (args.loop);

    path.stopReader();
}

// Spec 011 T7: --shards=N drives N fully independent EnginePath-equivalents (own ring, own
// matching thread pinned to its own core, own reader thread, own id space starting at 1 -- ids
// never collide because each shard has its own OrderBook/order-id map, exactly like N real
// gateway shards would). Each shard's population/warmup/measured/drain sequence is
// self-contained on its own thread; only `t0` and this function's aggregation step are shared,
// so a slow shard cannot perturb another shard's measurement -- the mechanical point of the
// isolation claim, reproduced here for the harness itself, not just runtime/shard.hpp.
struct ShardRunResult {
    std::size_t samples = 0;
    double achievedRate = 0.0;
    long long p50Ns = 0, p99Ns = 0, p999Ns = 0;
};

ShardRunResult runOneShard(const Args& args, Clock::time_point t0) {
    const Workload workload = args.workload == "thin" ? Workload::Thin : Workload::Dense;
    EnginePath path;

    OrderId nextId = 1;
    std::vector<ipc::Command> pop = populationCommands(workload, nextId);
    for (const auto& c : pop) {
        path.sendOne(c);
    }
    path.waitProcessed(pop.size());

    const bool saturation = (args.rate == "max");
    const std::int64_t intervalNs = saturation ? 0 : (1'000'000'000LL / std::stoll(args.rate));

    InflightTable inflight;
    LatencyRecorder recorder;
    std::atomic<std::size_t> completed{0};

    path.startReader(
        [&](OrderId id, std::int64_t completeNs) {
            inflight.recordComplete(id, completeNs);
            completed.fetch_add(1, std::memory_order_relaxed);
        },
        t0);

    SteadyStateGenerator gen(workload, thinTopPrice());
    Schedule schedule(t0, intervalNs > 0 ? intervalNs : 1);

    // See run()'s identical guard above: saturation mode has no schedule to be coordinatedly-
    // omitted against, so intendedNs must be actualSendNs, never 0 -- 0 turns correctedSpan into
    // an ever-growing "time since t0" fed at a 1ns interval into hdr_record_corrected_value(),
    // which is an O(span/interval) hang, not a slow run.
    auto sendIndexed = [&](std::size_t i) {
        const OrderId id = nextId++;
        if (!saturation) {
            Schedule::waitUntil(schedule.intendedTime(i));
        }
        const std::int64_t actualSendNs = nsSince(t0);
        const std::int64_t intendedNs = saturation ? actualSendNs : schedule.intendedNs(i);
        inflight.recordSend(id, intendedNs, actualSendNs);
        ipc::Command cmd = gen.next(id);
        path.sendOne(cmd);
    };

    for (std::size_t i = 0; i < args.warmup; ++i) {
        sendIndexed(i);
    }

    const OrderId measuredFirstId = nextId;
    const auto measureStart = Clock::now();
    for (std::size_t i = 0; i < args.samples; ++i) {
        sendIndexed(args.warmup + i);
    }
    const auto measureSendEnd = Clock::now();

    const std::size_t totalTracked = pop.size() + args.warmup + args.samples;
    const auto drainDeadline =
        Clock::now() + std::chrono::seconds(
                           std::max<long long>(60, static_cast<long long>(totalTracked) / 50 + 10));
    while (completed.load(std::memory_order_relaxed) < totalTracked &&
           Clock::now() < drainDeadline) {
        platform::cpuPause();
    }
    path.stopReader();

    recordRange(recorder, inflight, measuredFirstId, args.samples, intervalNs > 0 ? intervalNs : 1);

    ShardRunResult r;
    r.samples = args.samples;
    const double secs = std::chrono::duration<double>(measureSendEnd - measureStart).count();
    r.achievedRate = secs > 0.0 ? static_cast<double>(args.samples) / secs : 0.0;
    r.p50Ns = LatencyRecorder::percentile(recorder.corrected(), 50.0);
    r.p99Ns = LatencyRecorder::percentile(recorder.corrected(), 99.0);
    r.p999Ns =
        args.samples >= 1'000'000 ? LatencyRecorder::percentile(recorder.corrected(), 99.9) : 0;
    return r;
}

void runSharded(const Args& args) {
    const std::size_t numShards = args.shards;

    // 3 threads/shard: paced sender + pinned matching thread + reader thread (paths.hpp's doc).
    // Honest limit stated in the output, not just in the plan: past roughly hardware_concurrency
    // / 3 shards on a dev box, the curve measures the OS scheduler timeslicing real cores, not
    // the engine's actual per-shard isolation -- see specs/011-multi-instrument/spec.md.
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t threadsNeeded = numShards * 3;
    const bool oversubscribed = hw > 0 && threadsNeeded > static_cast<std::size_t>(hw);

    const auto t0 = Clock::now();
    std::vector<ShardRunResult> results(numShards);
    std::vector<std::thread> senders;
    senders.reserve(numShards);
    for (std::size_t i = 0; i < numShards; ++i) {
        senders.emplace_back([&, i] { results[i] = runOneShard(args, t0); });
    }
    for (auto& t : senders) {
        t.join();
    }

    double aggregateRate = 0.0;
    long long worstP50 = 0, worstP99 = 0, worstP999 = 0;
    for (const auto& r : results) {
        aggregateRate += r.achievedRate;
        worstP50 = std::max(worstP50, r.p50Ns);
        worstP99 = std::max(worstP99, r.p99Ns);
        worstP999 = std::max(worstP999, r.p999Ns);
    }

    std::printf("\n=== velox_loadgen: sharded engine path shards=%zu workload=%s ===\n", numShards,
                args.workload.c_str());
    std::printf("  threads needed:       %zu (3 per shard: sender + matching + reader)\n",
                threadsNeeded);
    std::printf("  hardware_concurrency: %u\n", hw);
    if (oversubscribed) {
        std::printf(
            "  *** %zu threads on %u cores -- OVERSUBSCRIBED. Past this N the curve measures\n"
            "      the scheduler, not the engine's per-shard isolation. ***\n",
            threadsNeeded, hw);
    }
    std::printf("  aggregate achieved rate: %.0f /sec\n", aggregateRate);
    std::printf("  worst-shard (max over N): p50 %8lld ns   p99 %8lld ns   p999 %s\n", worstP50,
                worstP99,
                args.samples >= 1'000'000 ? (std::to_string(worstP999) + " ns").c_str()
                                          : "NOT REPORTED (need >= 1,000,000 samples)");
    // Single grep-able line: scripts/shard_scaling.sh parses this rather than the prose above.
    std::printf(
        "SHARD_SCALING shards=%zu aggregate_ops_sec=%.0f worst_p50_ns=%lld worst_p99_ns=%lld "
        "worst_p999_ns=%lld oversubscribed=%d\n",
        numShards, aggregateRate, worstP50, worstP99, worstP999, oversubscribed ? 1 : 0);
    std::printf("======================================================================\n\n");

    if (!args.csvOut.empty()) {
        std::ofstream f(args.csvOut);
        f << "shards,aggregate_ops_sec,worst_shard_p50_ns,worst_shard_p99_ns,worst_shard_p999_"
             "ns,oversubscribed\n";
        f << numShards << "," << aggregateRate << "," << worstP50 << "," << worstP99 << ","
          << worstP999 << "," << (oversubscribed ? 1 : 0) << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    platform::prefaultPages();

    if (!args.replayJournal.empty()) {
        runReplay(args);
        return 0;
    }

    if (args.shards > 0) {
        if (args.path != "engine") {
            std::fprintf(stderr, "velox_loadgen: --shards requires --path=engine (got %s)\n",
                         args.path.c_str());
            return 2;
        }
        runSharded(args);
        return 0;
    }

    const std::string scenario =
        args.path + "_" + args.workload + (args.rate == "max" ? "_max" : "_" + args.rate);

    if (args.path == "engine") {
        EnginePath p;
        run(p, args, scenario);
    } else if (args.path == "durable") {
        DurablePath p(args.groupCommit);
        run(p, args, scenario);
    } else if (args.path == "wire") {
        WirePath p;
        run(p, args, scenario);
    } else {
        std::fprintf(stderr, "usage: velox_loadgen --path=engine|durable|wire [...]\n");
        return 2;
    }
    return 0;
}
