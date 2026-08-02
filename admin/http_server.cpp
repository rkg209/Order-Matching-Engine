#include "admin/http_server.hpp"

#include <sstream>

#include "common/http_parse.hpp"

namespace velox::admin {

namespace {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segs;
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) segs.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return segs;
}

bool matchPath(const std::vector<std::string>& pattern, const std::vector<std::string>& actual,
               std::unordered_map<std::string, std::string>& params) {
    if (pattern.size() != actual.size()) return false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const std::string& p = pattern[i];
        if (p.size() >= 2 && p.front() == '{' && p.back() == '}') {
            params[p.substr(1, p.size() - 2)] = actual[i];
        } else if (p != actual[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

void HttpServer::get(const std::string& pattern, RouteHandler handler) {
    routes_.push_back(Route{splitPath(pattern), std::move(handler)});
}

HttpResponse HttpServer::dispatch(HttpRequest& req) const {
    const std::vector<std::string> actual = splitPath(req.path);
    bool pathMatchedAnyMethod = false;
    for (const Route& route : routes_) {
        std::unordered_map<std::string, std::string> params;
        if (!matchPath(route.segments, actual, params)) continue;
        pathMatchedAnyMethod = true;
        if (req.method == "GET") {
            req.pathParams = std::move(params);
            return route.handler(req);
        }
    }
    if (pathMatchedAnyMethod) {
        return HttpResponse::json(405, R"({"error":"method_not_allowed"})");
    }
    return HttpResponse::json(404, R"({"error":"not_found"})");
}

namespace {

// One-shot: read the request line + headers, dispatch, write the response, close. No keep-alive.
class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
    HttpSession(asio::ip::tcp::socket socket, const HttpServer* server)
        : socket_(std::move(socket)), server_(server), inBuf_(HttpServer::kMaxHeaderBytes) {}

    void start() { readHeaders(); }

 private:
    void readHeaders() {
        auto self = shared_from_this();
        asio::async_read_until(socket_, inBuf_, "\r\n\r\n",
                               [self](std::error_code ec, std::size_t) {
                                   if (ec) {
                                       self->close();
                                       return;
                                   }
                                   self->handle();
                               });
    }

    void handle() {
        std::istream is(&inBuf_);
        std::string requestLine;
        std::getline(is, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

        std::ostringstream rest;
        rest << is.rdbuf();

        HttpRequest req;
        std::string target, version;
        {
            std::istringstream ss(requestLine);
            ss >> req.method >> target >> version;
        }
        const std::size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = target;
        } else {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
        req.headers = common::parseHeaders(rest.str());

        const HttpResponse resp = server_->dispatch(req);
        writeResponse(resp);
    }

    void writeResponse(const HttpResponse& resp) {
        static const std::unordered_map<int, std::string> kReasons = {
            {200, "OK"},        {400, "Bad Request"}, {401, "Unauthorized"},
            {403, "Forbidden"}, {404, "Not Found"},   {405, "Method Not Allowed"},
        };
        const auto it = kReasons.find(resp.status);
        const std::string reason = it != kReasons.end() ? it->second : "Error";

        std::ostringstream out;
        out << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
            << "Content-Type: " << resp.contentType << "\r\n"
            << "Content-Length: " << resp.body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << resp.body;

        auto self = shared_from_this();
        auto buf = std::make_shared<std::string>(out.str());
        asio::async_write(socket_, asio::buffer(*buf),
                          [self, buf](std::error_code, std::size_t) { self->close(); });
    }

    void close() {
        std::error_code ec;
        socket_.close(ec);
    }

    asio::ip::tcp::socket socket_;
    const HttpServer* server_;
    asio::streambuf inBuf_;
};

}  // namespace

void HttpServer::listen(unsigned short port, const std::string& bindAddr) {
    const asio::ip::address addr = asio::ip::make_address(bindAddr);
    asio::ip::tcp::endpoint ep(addr, port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    doAccept();
}

void HttpServer::doAccept() {
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<HttpSession>(std::move(socket), this);
            session->start();
        }
        doAccept();
    });
}

}  // namespace velox::admin
