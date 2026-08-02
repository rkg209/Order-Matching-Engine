#include "admin/routes.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "admin/json.hpp"
#include "admin/jwt.hpp"

namespace velox::admin {

namespace {

constexpr std::int64_t kDefaultLimit = 100;
constexpr std::int64_t kMaxLimit = 1000;

std::unordered_map<std::string, std::string> parseQuery(const std::string& query) {
    std::unordered_map<std::string, std::string> out;
    std::size_t start = 0;
    while (start < query.size()) {
        std::size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(start, amp - start);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        } else if (!pair.empty()) {
            out[pair] = "";
        }
        start = amp + 1;
    }
    return out;
}

bool isAllDigits(const std::string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::int64_t queryInt(const std::unordered_map<std::string, std::string>& q, const std::string& key,
                      std::int64_t def) {
    const auto it = q.find(key);
    if (it == q.end() || !isAllDigits(it->second)) return def;
    return std::stoll(it->second);
}

// Bearer-token auth: any of ADMIN/OPERATIONS/READ_ONLY authorizes every route this daemon
// exposes -- there is no route here that needs a narrower role, since nothing mutates (plan
// decision: "Authorization: roles").
struct AuthOutcome {
    bool ok = false;
    HttpResponse rejection;
};

AuthOutcome authorize(const HttpRequest& req, const std::string& jwtSecret,
                      const std::unordered_set<std::string>& revokedJti) {
    const auto it = req.headers.find("authorization");
    if (it == req.headers.end() || it->second.rfind("Bearer ", 0) != 0) {
        return {false, HttpResponse::json(401, R"({"error":"missing_bearer_token"})")};
    }
    const std::string token = it->second.substr(7);
    const VerifyResult r =
        verifyJwt(token, jwtSecret, static_cast<std::int64_t>(std::time(nullptr)), revokedJti);
    if (!r.ok) {
        return {false, HttpResponse::json(401, R"({"error":"invalid_token"})")};
    }
    static const std::unordered_set<std::string> kValidRoles = {"ADMIN", "OPERATIONS", "READ_ONLY"};
    const bool hasValidRole =
        std::any_of(r.roles.begin(), r.roles.end(),
                    [](const std::string& role) { return kValidRoles.count(role) > 0; });
    if (!hasValidRole) {
        return {false, HttpResponse::json(403, R"({"error":"insufficient_role"})")};
    }
    return {true, HttpResponse{}};
}

bool parseInstrumentId(const std::string& raw, std::uint32_t& out) {
    if (!isAllDigits(raw)) return false;
    out = static_cast<std::uint32_t>(std::stoul(raw));
    return true;
}

}  // namespace

void registerAdminRoutes(HttpServer& server, AdminStore& store, const std::string& jwtSecret,
                         const std::unordered_set<std::string>& revokedJti) {
    server.get("/healthz",
               [](const HttpRequest&) { return HttpResponse::json(200, R"({"status":"ok"})"); });

    server.get("/api/v1/engine/status", [&store, jwtSecret, &revokedJti](const HttpRequest& req) {
        const AuthOutcome auth = authorize(req, jwtSecret, revokedJti);
        if (!auth.ok) return auth.rejection;

        JsonWriter w;
        w.beginObject();
        w.key("source").value(std::string_view("disk"));
        w.key("shards").beginArray();
        for (const InstrumentInfo& inst : store.listInstruments()) {
            const EngineStatus st = store.engineStatus(inst);
            w.beginObject();
            w.key("instrument_id").value(static_cast<std::int64_t>(inst.id));
            w.key("last_good_seq").value(st.lastGoodSeq);
            w.key("segment_count").value(static_cast<std::int64_t>(st.segmentCount));
            w.key("snapshot_count").value(static_cast<std::int64_t>(st.snapshotCount));
            w.key("newest_snapshot_seq").value(st.newestSnapshotSeq);
            w.key("journal_bytes").value(static_cast<std::int64_t>(st.journalBytes));
            w.key("open_segment").value(st.openSegment);
            w.endObject();
        }
        w.endArray();
        w.endObject();
        return HttpResponse::json(200, w.str());
    });

    server.get("/api/v1/instruments", [&store, jwtSecret, &revokedJti](const HttpRequest& req) {
        const AuthOutcome auth = authorize(req, jwtSecret, revokedJti);
        if (!auth.ok) return auth.rejection;

        const auto q = parseQuery(req.query);
        const std::int64_t limit = std::min(queryInt(q, "limit", kDefaultLimit), kMaxLimit);
        const std::int64_t after = queryInt(q, "after", -1);

        const std::vector<InstrumentInfo> all = store.listInstruments();
        JsonWriter w;
        w.beginObject();
        w.key("items").beginArray();
        std::int64_t lastId = after;
        bool hasMore = false;
        std::int64_t emitted = 0;
        for (const InstrumentInfo& inst : all) {
            if (static_cast<std::int64_t>(inst.id) <= after) continue;
            if (emitted >= limit) {
                hasMore = true;
                break;
            }
            w.beginObject();
            w.key("id").value(static_cast<std::int64_t>(inst.id));
            w.endObject();
            lastId = static_cast<std::int64_t>(inst.id);
            ++emitted;
        }
        w.endArray();
        w.key("next_cursor");
        if (hasMore) {
            w.value(lastId);
        } else {
            w.nullValue();
        }
        w.key("has_more").value(hasMore);
        w.endObject();
        return HttpResponse::json(200, w.str());
    });

    server.get("/api/v1/instruments/{id}/journal/segments", [&store, jwtSecret,
                                                             &revokedJti](const HttpRequest& req) {
        const AuthOutcome auth = authorize(req, jwtSecret, revokedJti);
        if (!auth.ok) return auth.rejection;

        std::uint32_t id = 0;
        if (!parseInstrumentId(req.pathParams.at("id"), id)) {
            return HttpResponse::json(400, R"({"error":"invalid_instrument_id"})");
        }
        const auto inst = store.findInstrument(id);
        if (!inst) {
            return HttpResponse::json(404, R"({"error":"instrument_not_found"})");
        }

        const auto q = parseQuery(req.query);
        const std::int64_t limit = std::min(queryInt(q, "limit", kDefaultLimit), kMaxLimit);
        const std::int64_t after = queryInt(q, "after", -1);

        const std::vector<SegmentInfo> segs = store.listSegments(*inst);
        JsonWriter w;
        w.beginObject();
        w.key("items").beginArray();
        std::int64_t lastSeq = after;
        bool hasMore = false;
        std::int64_t emitted = 0;
        for (const SegmentInfo& s : segs) {
            if (s.firstSeq <= after) continue;
            if (emitted >= limit) {
                hasMore = true;
                break;
            }
            w.beginObject();
            w.key("file_name").value(std::string_view(s.fileName));
            w.key("first_seq").value(s.firstSeq);
            w.key("created_counter").value(static_cast<std::int64_t>(s.createdCounter));
            w.key("bytes").value(static_cast<std::int64_t>(s.bytes));
            w.key("header_valid").value(s.headerValid);
            w.endObject();
            lastSeq = s.firstSeq;
            ++emitted;
        }
        w.endArray();
        w.key("next_cursor");
        if (hasMore) {
            w.value(lastSeq);
        } else {
            w.nullValue();
        }
        w.key("has_more").value(hasMore);
        w.endObject();
        return HttpResponse::json(200, w.str());
    });

    server.get("/api/v1/instruments/{id}/snapshots", [&store, jwtSecret,
                                                      &revokedJti](const HttpRequest& req) {
        const AuthOutcome auth = authorize(req, jwtSecret, revokedJti);
        if (!auth.ok) return auth.rejection;

        std::uint32_t id = 0;
        if (!parseInstrumentId(req.pathParams.at("id"), id)) {
            return HttpResponse::json(400, R"({"error":"invalid_instrument_id"})");
        }
        const auto inst = store.findInstrument(id);
        if (!inst) {
            return HttpResponse::json(404, R"({"error":"instrument_not_found"})");
        }

        const auto q = parseQuery(req.query);
        const std::int64_t limit = std::min(queryInt(q, "limit", kDefaultLimit), kMaxLimit);
        const std::int64_t after = queryInt(q, "after", -1);

        const std::vector<SnapshotInfo> snaps = store.listSnapshots(*inst);
        JsonWriter w;
        w.beginObject();
        w.key("items").beginArray();
        std::int64_t lastSeq = after;
        bool hasMore = false;
        std::int64_t emitted = 0;
        for (const SnapshotInfo& s : snaps) {
            if (s.globalSeq <= after) continue;
            if (emitted >= limit) {
                hasMore = true;
                break;
            }
            w.beginObject();
            w.key("file_name").value(std::string_view(s.fileName));
            w.key("global_seq").value(s.globalSeq);
            w.key("book_seq").value(s.bookSeq);
            w.key("order_count").value(static_cast<std::int64_t>(s.orderCount));
            w.key("bytes").value(static_cast<std::int64_t>(s.bytes));
            w.key("crc_valid").value(s.crcValid);
            w.endObject();
            lastSeq = s.globalSeq;
            ++emitted;
        }
        w.endArray();
        w.key("next_cursor");
        if (hasMore) {
            w.value(lastSeq);
        } else {
            w.nullValue();
        }
        w.key("has_more").value(hasMore);
        w.endObject();
        return HttpResponse::json(200, w.str());
    });
}

}  // namespace velox::admin
