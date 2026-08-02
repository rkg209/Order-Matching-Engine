#pragma once

// Spec 013 T4: a small stack-based JSON emitter with proper string escaping -- admin/routes.cpp's
// responses include values that did not originate as compile-time literals (instrument ids from
// the URL path, `sub`/`roles` out of a verified JWT), unlike viz/snapshot_json.hpp's fully-typed
// numeric/enum fields, so escaping is not optional here the way it was there.
//
// Same rule as viz/snapshot_json.hpp: prices and every other on-disk integer stay scaled `int64`
// on the wire. No floating point crosses this boundary.

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace velox::admin {

class JsonWriter {
 public:
    JsonWriter& beginObject() {
        beforeValue();
        out_ << '{';
        stack_.push_back({true, true});
        return *this;
    }

    JsonWriter& endObject() {
        out_ << '}';
        stack_.pop_back();
        return *this;
    }

    JsonWriter& beginArray() {
        beforeValue();
        out_ << '[';
        stack_.push_back({false, true});
        return *this;
    }

    JsonWriter& endArray() {
        out_ << ']';
        stack_.pop_back();
        return *this;
    }

    // Object member name. Only valid directly inside beginObject()/endObject().
    JsonWriter& key(std::string_view k) {
        if (!stack_.back().first) out_ << ',';
        stack_.back().first = false;
        out_ << '"' << escape(k) << "\":";
        return *this;
    }

    JsonWriter& value(std::string_view s) {
        beforeValue();
        out_ << '"' << escape(s) << '"';
        return *this;
    }

    JsonWriter& value(std::int64_t n) {
        beforeValue();
        out_ << n;
        return *this;
    }

    JsonWriter& value(std::uint64_t n) {
        beforeValue();
        out_ << n;
        return *this;
    }

    JsonWriter& value(bool b) {
        beforeValue();
        out_ << (b ? "true" : "false");
        return *this;
    }

    JsonWriter& nullValue() {
        beforeValue();
        out_ << "null";
        return *this;
    }

    std::string str() const { return out_.str(); }

 private:
    struct Frame {
        bool isObject;
        bool first;
    };

    // Inserts the comma an array needs between elements. Object members handle their own comma
    // in key() (which always runs before value()/beginObject()/beginArray() for that member), so
    // this is a no-op when the enclosing container is an object.
    void beforeValue() {
        if (!stack_.empty() && !stack_.back().isObject) {
            if (!stack_.back().first) out_ << ',';
            stack_.back().first = false;
        }
    }

    static std::string escape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (const char c : s) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    std::ostringstream out_;
    std::vector<Frame> stack_;
};

}  // namespace velox::admin
