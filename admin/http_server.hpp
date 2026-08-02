#pragma once

// Spec 013 T5: a minimal Asio HTTP/1.1 server, GET-only by construction -- the route table only
// ever registers GET handlers (admin/routes.cpp), and any other method that matches a known path
// gets a 405 without ever reaching a handler. Framing follows the same request-line/header-block
// shape viz/ws_server.cpp already uses for its embedded static-file server, minus the WebSocket
// upgrade path this daemon has no use for.
//
// Every connection is `Connection: close` -- this is a low-QPS control-plane surface (durability-
// plane status, not the order gateway), so there is no benefit to keep-alive complexity here.

#include <asio.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace velox::admin {

struct HttpRequest {
    std::string method;
    std::string path;   // request-target with any query string stripped
    std::string query;  // raw query string, no leading '?'
    std::unordered_map<std::string, std::string> headers;     // lower-cased keys
    std::unordered_map<std::string, std::string> pathParams;  // {id} -> "7"
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;

    static HttpResponse json(int status, std::string body) {
        return HttpResponse{status, "application/json", std::move(body)};
    }
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
    static constexpr std::size_t kMaxHeaderBytes = 8192;

    explicit HttpServer(asio::io_context& io) : acceptor_(io) {}

    // Registers a GET route. `pattern` segments starting with '{' and ending with '}' are path
    // parameters (e.g. "/api/v1/instruments/{id}/snapshots"); every other segment must match the
    // request path literally.
    void get(const std::string& pattern, RouteHandler handler);

    void listen(unsigned short port, const std::string& bindAddr);
    unsigned short localPort() const { return acceptor_.local_endpoint().port(); }

    // Dispatches a parsed request against the route table. Exposed so tests can drive routing
    // logic directly; also used internally by the accepted-connection path.
    HttpResponse dispatch(HttpRequest& req) const;

 private:
    struct Route {
        std::vector<std::string> segments;  // "{id}" kept as-is; matchPath() interprets it
        RouteHandler handler;
    };

    void doAccept();

    asio::ip::tcp::acceptor acceptor_;
    std::vector<Route> routes_;
};

}  // namespace velox::admin
