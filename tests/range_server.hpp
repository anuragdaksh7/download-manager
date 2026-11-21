#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test_support {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
inline void closeSocket(Socket s) { ::close(s); }
#endif

struct ServerBehaviour {
    bool advertiseRanges = true;  // send "Accept-Ranges: bytes" on HEAD
    bool honourRanges = true;     // answer Range requests with 206 instead of the full body
    int headStatus = 200;
    std::chrono::milliseconds rangeDelay{0};  // hold each 206 response open this long
    bool omitContentLength = false;           // leave Content-Length off HEAD responses
    bool lowercaseHeaders = false;            // send header names in lowercase, as HTTP/2 does
};

// Minimal HTTP/1.1 server on 127.0.0.1 serving one in-memory file, one thread per connection.
class RangeServer {
public:
    explicit RangeServer(std::string body, ServerBehaviour behaviour = {})
        : body_(std::move(body)), behaviour_(behaviour) {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kInvalidSocket) {
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        socklen_t len = sizeof(addr);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_, 64) != 0 ||
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            closeSocket(listener_);
            throw std::runtime_error("failed to listen on loopback");
        }
        port_ = ntohs(addr.sin_port);

        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    RangeServer(const RangeServer&) = delete;
    RangeServer& operator=(const RangeServer&) = delete;

    ~RangeServer() {
        stopping_ = true;
#ifndef _WIN32
        ::shutdown(listener_, SHUT_RDWR);
#endif
        closeSocket(listener_);
        acceptThread_.join();
        for (auto& handler : handlers_) {
            handler.join();
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/file.bin";
    }

    [[nodiscard]] int rangeRequests() const { return rangeRequests_.load(); }
    [[nodiscard]] int maxConcurrentRanges() const { return maxActive_.load(); }

private:
    std::string body_;
    ServerBehaviour behaviour_;
    Socket listener_ = kInvalidSocket;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptThread_;
    std::vector<std::thread> handlers_;  // only touched by the accept thread until it is joined
    std::atomic<int> rangeRequests_{0};
    std::atomic<int> active_{0};
    std::atomic<int> maxActive_{0};

    void acceptLoop() {
        while (!stopping_) {
            const Socket client = ::accept(listener_, nullptr, nullptr);
            if (client == kInvalidSocket) {
                break;
            }
            handlers_.emplace_back([this, client] {
                handle(client);
                closeSocket(client);
            });
        }
    }

    static std::optional<std::string> headerValue(const std::string& request, std::string_view name) {
        std::string lower = request;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const auto pos = lower.find("\r\n" + std::string(name) + ":");
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        auto start = pos + 2 + name.size() + 1;
        const auto end = request.find("\r\n", start);
        while (start < end && request[start] == ' ') {
            ++start;
        }
        return request.substr(start, end - start);
    }

    static bool sendAll(Socket s, std::string_view data) {
        while (!data.empty()) {
            const auto chunk = std::min<std::size_t>(data.size(), 1 << 16);
#ifdef _WIN32
            const int sent = ::send(s, data.data(), static_cast<int>(chunk), 0);
#else
            const auto sent = ::send(s, data.data(), chunk, MSG_NOSIGNAL);
#endif
            if (sent <= 0) {
                return false;
            }
            data.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    void respond(Socket s, int status, std::string_view reason, const std::string& extraHeaders,
                 std::string_view payload, bool includeBody) const {
        std::string headers = "Connection: close\r\n" + extraHeaders;
        if (includeBody || !behaviour_.omitContentLength) {
            headers += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        }
        if (behaviour_.lowercaseHeaders) {
            std::transform(headers.begin(), headers.end(), headers.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        const std::string head =
            "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n" + headers + "\r\n";
        if (sendAll(s, head) && includeBody) {
            sendAll(s, payload);
        }
    }

    void handle(Socket client) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return;
            }
            request.append(buffer, static_cast<std::size_t>(n));
        }

        const std::string method = request.substr(0, request.find(' '));

        if (method == "HEAD") {
            if (behaviour_.headStatus != 200) {
                respond(client, behaviour_.headStatus, "Error", "", "", false);
                return;
            }
            std::string headers = "ETag: \"test-etag\"\r\n";
            if (behaviour_.advertiseRanges) {
                headers += "Accept-Ranges: bytes\r\n";
            }
            respond(client, 200, "OK", headers, body_, false);
            return;
        }

        const auto range = headerValue(request, "range");
        if (!range || !behaviour_.honourRanges || range->rfind("bytes=", 0) != 0 || body_.empty()) {
            respond(client, 200, "OK", "", body_, true);
            return;
        }

        const auto dash = range->find('-');
        const std::uint64_t last = body_.size() - 1;
        const std::uint64_t start = std::stoull(range->substr(6, dash - 6));
        const std::string endText = range->substr(dash + 1);
        const std::uint64_t end = endText.empty() ? last : std::min<std::uint64_t>(std::stoull(endText), last);

        ++rangeRequests_;
        const int active = ++active_;
        int previous = maxActive_.load();
        while (previous < active && !maxActive_.compare_exchange_weak(previous, active)) {
        }
        std::this_thread::sleep_for(behaviour_.rangeDelay);

        const std::string contentRange = "Content-Range: bytes " + std::to_string(start) + "-" +
                                         std::to_string(end) + "/" + std::to_string(body_.size()) + "\r\n";
        respond(client, 206, "Partial Content", contentRange,
                std::string_view(body_).substr(static_cast<std::size_t>(start),
                                               static_cast<std::size_t>(end - start + 1)),
                true);
        --active_;
    }
};

}  // namespace test_support
