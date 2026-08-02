#pragma once

// Spec 013 T5: wires AdminStore + JWT verification into the HttpServer route table. Every route
// registered here is GET; admin/http_server.cpp already turns anything else into a 405 before a
// handler is ever reached.

#include <string>
#include <unordered_set>

#include "admin/http_server.hpp"
#include "admin/store.hpp"

namespace velox::admin {

// `revokedJti` is read at request time, not copied in -- the caller (apps/velox_adminctl.cpp)
// mutates it in place on SIGHUP, and since asio runs single-threaded here that is race-free.
void registerAdminRoutes(HttpServer& server, AdminStore& store, const std::string& jwtSecret,
                         const std::unordered_set<std::string>& revokedJti);

}  // namespace velox::admin
