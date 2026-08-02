#include "viz/ws_server.hpp"

#include <cstring>
#include <sstream>

#include "common/http_parse.hpp"
#include "viz/base64.hpp"
#include "viz/sha1.hpp"

namespace velox::viz {

namespace {

using velox::common::parseHeaders;
using velox::common::toLower;

constexpr const char* kWsMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

}  // namespace

// --- WsSession ----------------------------------------------------------------------------

void WsSession::start() {
    readHeaders();
}

void WsSession::readHeaders() {
    auto self = shared_from_this();
    asio::async_read_until(socket_, inBuf_, "\r\n\r\n", [self](std::error_code ec, std::size_t) {
        if (ec) {
            self->close();
            return;
        }
        std::istream is(&self->inBuf_);
        std::string requestLine;
        std::getline(is, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

        std::ostringstream rest;
        rest << is.rdbuf();
        self->handleRequestLine(requestLine, rest.str());
    });
}

void WsSession::handleRequestLine(const std::string& requestLine, const std::string& headerBlock) {
    // "GET /path HTTP/1.1"
    std::string method, path;
    {
        std::istringstream ss(requestLine);
        std::string version;
        ss >> method >> path >> version;
    }

    const auto headers = parseHeaders(headerBlock);
    const auto upgradeIt = headers.find("upgrade");
    const auto keyIt = headers.find("sec-websocket-key");
    const bool wantsUpgrade =
        upgradeIt != headers.end() && toLower(upgradeIt->second) == "websocket";

    if (method == "GET" && wantsUpgrade && keyIt != headers.end()) {
        completeHandshake(keyIt->second);
        return;
    }
    serveHttp(path);
}

void WsSession::completeHandshake(const std::string& secWebSocketKey) {
    const std::string acceptKey = computeAcceptKey(secWebSocketKey);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << acceptKey << "\r\n\r\n";
    const std::string body = resp.str();

    auto self = shared_from_this();
    auto buf = std::make_shared<std::string>(body);
    asio::async_write(socket_, asio::buffer(*buf), [self, buf](std::error_code ec, std::size_t) {
        if (ec) {
            self->close();
            return;
        }
        self->isWs_ = true;
        self->readFrameHeader();
    });
}

void WsSession::serveHttp(const std::string& urlPath) {
    std::string body, contentType;
    const bool found = staticFiles_->load(urlPath, body, contentType);

    std::ostringstream resp;
    if (found) {
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    } else {
        const std::string notFound = "not found\n";
        resp << "HTTP/1.1 404 Not Found\r\n"
             << "Content-Type: text/plain\r\n"
             << "Content-Length: " << notFound.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << notFound;
    }

    auto self = shared_from_this();
    auto buf = std::make_shared<std::string>(resp.str());
    asio::async_write(socket_, asio::buffer(*buf),
                      [self, buf](std::error_code, std::size_t) { self->close(); });
}

// --- WS frame reading (client -> server): parsed just enough to route CLOSE/PING, everything
// else is read and discarded. Client frames are always masked (RFC6455 5.1) -- an unmasked
// client frame is a protocol violation and this closes the connection rather than accept it.

void WsSession::readFrameHeader() {
    auto self = shared_from_this();
    asio::async_read(socket_, inBuf_, asio::transfer_exactly(2),
                     [self](std::error_code ec, std::size_t) {
                         if (ec) {
                             self->close();
                             return;
                         }
                         std::istream is(&self->inBuf_);
                         std::uint8_t b0 = 0, b1 = 0;
                         is.read(reinterpret_cast<char*>(&b0), 1);
                         is.read(reinterpret_cast<char*>(&b1), 1);
                         self->opcode_ = b0 & 0x0F;
                         self->masked_ = (b1 & 0x80) != 0;
                         if (!self->masked_) {
                             self->close();  // protocol violation -- see doc above
                             return;
                         }
                         self->readExtendedLength(b1);
                     });
}

void WsSession::readExtendedLength(std::uint8_t byte1) {
    const std::uint8_t len7 = byte1 & 0x7F;
    if (len7 <= 125) {
        payloadLen_ = len7;
        readMaskKey();
        return;
    }
    const std::size_t extraBytes = (len7 == 126) ? 2 : 8;
    auto self = shared_from_this();
    asio::async_read(socket_, inBuf_, asio::transfer_exactly(extraBytes),
                     [self, extraBytes](std::error_code ec, std::size_t) {
                         if (ec) {
                             self->close();
                             return;
                         }
                         std::istream is(&self->inBuf_);
                         std::uint64_t len = 0;
                         for (std::size_t i = 0; i < extraBytes; ++i) {
                             std::uint8_t b = 0;
                             is.read(reinterpret_cast<char*>(&b), 1);
                             len = (len << 8) | b;
                         }
                         self->payloadLen_ = len;
                         self->readMaskKey();
                     });
}

void WsSession::readMaskKey() {
    auto self = shared_from_this();
    asio::async_read(socket_, inBuf_, asio::transfer_exactly(4),
                     [self](std::error_code ec, std::size_t) {
                         if (ec) {
                             self->close();
                             return;
                         }
                         std::istream is(&self->inBuf_);
                         is.read(reinterpret_cast<char*>(self->maskKey_.data()), 4);
                         self->readPayload();
                     });
}

void WsSession::readPayload() {
    // CLOSE/PING control frames are capped at 125 bytes by the RFC; anything else this server
    // cares to inspect is small. A very large data frame (which this feed never legitimately
    // sends anyway) is still read fully -- transfer_exactly bounds it to payloadLen_, never
    // unbounded -- but not copied anywhere costly.
    if (payloadLen_ == 0) {
        handleFrame();
        return;
    }
    auto self = shared_from_this();
    asio::async_read(socket_, inBuf_, asio::transfer_exactly(payloadLen_),
                     [self](std::error_code ec, std::size_t) {
                         if (ec) {
                             self->close();
                             return;
                         }
                         self->payloadBuf_.resize(self->payloadLen_);
                         std::istream is(&self->inBuf_);
                         is.read(reinterpret_cast<char*>(self->payloadBuf_.data()),
                                 static_cast<std::streamsize>(self->payloadLen_));
                         // Unmask (RFC6455 5.3) -- only needed for CLOSE/PING inspection below,
                         // but cheap enough to always do rather than branch on opcode first.
                         for (std::size_t i = 0; i < self->payloadBuf_.size(); ++i) {
                             self->payloadBuf_[i] ^= std::byte{self->maskKey_[i % 4]};
                         }
                         self->handleFrame();
                     });
}

void WsSession::handleFrame() {
    constexpr std::uint8_t kOpClose = 0x8;
    constexpr std::uint8_t kOpPing = 0x9;

    if (opcode_ == kOpClose) {
        close();
        return;
    }
    if (opcode_ == kOpPing) {
        std::vector<std::byte> frame;
        frame.push_back(std::byte{0x8A});  // FIN + pong
        frame.push_back(static_cast<std::byte>(payloadBuf_.size() & 0x7F));
        frame.insert(frame.end(), payloadBuf_.begin(), payloadBuf_.end());
        enqueueRaw(std::move(frame));
    }
    payloadBuf_.clear();
    if (!closed_) {
        readFrameHeader();
    }
}

// --- server -> client: text frames only ----------------------------------------------------

void WsSession::sendText(const std::string& payload) {
    if (!isWs_ || closed_) return;
    enqueueRaw(encodeTextFrame(payload));
}

std::vector<std::byte> encodeTextFrame(const std::string& payload) {
    std::vector<std::byte> frame;
    const std::size_t len = payload.size();
    frame.push_back(std::byte{0x81});  // FIN + text opcode
    if (len <= 125) {
        frame.push_back(static_cast<std::byte>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(std::byte{126});
        frame.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
        frame.push_back(static_cast<std::byte>(len & 0xFF));
    } else {
        frame.push_back(std::byte{127});
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<std::byte>((len >> (8 * i)) & 0xFF));
        }
    }
    const auto* p = reinterpret_cast<const std::byte*>(payload.data());
    frame.insert(frame.end(), p, p + len);
    return frame;
}

std::string computeAcceptKey(const std::string& secWebSocketKey) {
    const auto digest = sha1(secWebSocketKey + kWsMagicGuid);
    return base64Encode(digest);
}

void WsSession::enqueueRaw(std::vector<std::byte> frame) {
    if (closed_) return;
    if (queuedBytes_ + frame.size() > maxQueueBytes_) {
        close();  // same overflow policy as marketdata::FeedSession::enqueue()
        return;
    }
    queuedBytes_ += frame.size();
    pending_.push_back(std::move(frame));
    if (!writing_) {
        doWrite();
    }
}

void WsSession::doWrite() {
    if (pending_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(pending_.front()),
                      [self](std::error_code ec, std::size_t n) {
                          if (ec) {
                              self->close();
                              return;
                          }
                          self->queuedBytes_ -= n;
                          self->pending_.pop_front();
                          self->doWrite();
                      });
}

void WsSession::close() {
    closed_ = true;
    std::error_code ec;
    socket_.close(ec);
}

// --- WsServer -------------------------------------------------------------------------------

void WsServer::listen(unsigned short port) {
    asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    doAccept();
}

void WsServer::doAccept() {
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<WsSession>(std::move(socket), &staticFiles_);
            session->start();
            sessions_.push_back(session);
        }
        doAccept();
    });
}

void WsServer::broadcastText(const std::string& payload) {
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if ((*it)->closed()) {
            it = sessions_.erase(it);
            continue;
        }
        if ((*it)->isWs()) {
            (*it)->sendText(payload);
        }
        ++it;
    }
}

}  // namespace velox::viz
