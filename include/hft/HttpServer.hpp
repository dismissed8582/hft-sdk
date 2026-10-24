#pragma once

#include <atomic>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hft::http {

struct Request {
    std::string method;
    std::string path;
};

struct Response {
    int status{200};
    std::string content_type{"text/plain; version=0.0.4"};
    std::string body;
};

class Server {
public:
    using Handler = std::function<Response(const Request&)>;

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server() { stop(); }

    bool start(const std::string& address, std::uint16_t port, Handler handler) {
        if (running_.exchange(true)) {
            return true;
        }
        handler_ = std::move(handler);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(),
                        &hints, &result) != 0) {
            running_.store(false);
            return false;
        }
        int descriptor = -1;
        for (addrinfo* candidate = result; candidate != nullptr;
             candidate = candidate->ai_next) {
            descriptor = ::socket(candidate->ai_family, candidate->ai_socktype,
                                  candidate->ai_protocol);
            if (descriptor < 0) {
                continue;
            }
            int reuse = 1;
            ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof(reuse));
            if (::bind(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
                ::listen(descriptor, 16) == 0) {
                break;
            }
            ::close(descriptor);
            descriptor = -1;
        }
        freeaddrinfo(result);
        if (descriptor < 0) {
            running_.store(false);
            return false;
        }
        socket_ = descriptor;
        thread_ = std::thread([this]() { serve(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (socket_ >= 0) {
            ::shutdown(socket_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
    }

private:
    void serve() {
        while (running_.load(std::memory_order_acquire)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket_, &readable);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200'000;
            const int ready = ::select(socket_ + 1, &readable, nullptr, nullptr,
                                       &timeout);
            if (ready <= 0) {
                continue;
            }
            sockaddr_storage peer{};
            socklen_t peer_length = sizeof(peer);
            const int client = ::accept(socket_,
                                        reinterpret_cast<sockaddr*>(&peer),
                                        &peer_length);
            if (client < 0) {
                continue;
            }
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        char buffer[4096];
        const ssize_t received = ::recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            return;
        }
        buffer[received] = '\0';
        std::istringstream input(buffer);
        Request request;
        std::string version;
        input >> request.method >> request.path >> version;
        const Response response = handler_ ? handler_(request)
                                           : Response{404, "text/plain", "not found\n"};
        std::ostringstream output;
        output << "HTTP/1.1 " << response.status << ' '
               << (response.status == 200 ? "OK" : "Not Found") << "\r\n"
               << "Content-Type: " << response.content_type << "\r\n"
               << "Content-Length: " << response.body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << response.body;
        const std::string text = output.str();
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t sent = ::send(client, text.data() + written,
                                        text.size() - written, 0);
            if (sent <= 0) {
                break;
            }
            written += static_cast<std::size_t>(sent);
        }
    }

    std::atomic<bool> running_{false};
    int socket_{-1};
    std::thread thread_;
    Handler handler_;
};

} // namespace hft::http
