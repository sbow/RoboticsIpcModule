#pragma once

#include "buffer.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct SocketConfig {
    const int domain;
    const int data_model_type;
    const int protocol;

    constexpr SocketConfig(int d, int type, int p) : domain(d), data_model_type(type), protocol(p) {}
};

class Socket {
public:
    explicit Socket(SocketConfig cfg)
        : fd_(::socket(cfg.domain, cfg.data_model_type, cfg.protocol)) {
        if (fd_ < 0) {
            throw std::runtime_error("failed to create socket");
        }
    }

    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int fd() const { return fd_; }

private:
    int fd_;
};

inline void datagram_set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::runtime_error("setsockopt SO_RCVTIMEO failed");
    }
}

inline void datagram_set_reuseaddr(int fd) {
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        throw std::runtime_error("setsockopt SO_REUSEADDR failed");
    }
}

struct Udp {
    using Handle = Socket;

    struct BindParams {
        uint16_t port;
    };

    struct SendParams {
        const char* host = "127.0.0.1";
        uint16_t port;
        Buffer payload;
    };

    struct RecvResult {
        sockaddr_in from{};
    };

    static constexpr SocketConfig kCfg{AF_INET, SOCK_DGRAM, IPPROTO_UDP};

    static Handle open() { return Handle(kCfg); }

    static int native_fd(const Handle& handle) { return handle.fd(); }

    static void set_recv_timeout(const Handle& handle, int ms) {
        datagram_set_recv_timeout(handle.fd(), ms);
    }

    static void bind(const Handle& handle, const BindParams& params) {
        datagram_set_reuseaddr(handle.fd());

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(params.port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(handle.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("failed to bind socket");
        }
    }

    static void send_to(int fd, const sockaddr_in& dest, const Buffer& payload) {
        ssize_t n = ::sendto(fd, payload.data, payload.size, 0,
            reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (n < 0) {
            throw std::runtime_error("udp sendto failed");
        }
    }

    static void send(const Handle& handle, const SendParams& params, const Buffer& payload) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(params.port);
        if (inet_pton(AF_INET, params.host, &addr.sin_addr) <= 0) {
            throw std::runtime_error("invalid host");
        }
        send_to(handle.fd(), addr, payload);
    }

    static void send_to(int fd, const char* host, uint16_t port, const Buffer& payload) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            throw std::runtime_error("invalid host");
        }
        send_to(fd, addr, payload);
    }

    static void recv(const Handle& handle, Buffer& buf, RecvResult& out) {
        socklen_t from_len = sizeof(out.from);

        const ssize_t n = ::recvfrom(handle.fd(), buf.data, buf.capacity, 0,
            reinterpret_cast<sockaddr*>(&out.from), &from_len);

        if (n < 0) {
            throw std::runtime_error("udp recvfrom failed");
        }
        buf.size = static_cast<size_t>(n);
    }

    static void connect_or_send(const Handle& handle, const SendParams& params) {
        send(handle, params, params.payload);
    }

    static void echo(const Handle& handle, const RecvResult& r, const Buffer& payload) {
        send_to(handle.fd(), r.from, payload);
    }

    // Legacy names used by some tests.
    static void recv_from(const Handle& handle, Buffer& buf, RecvResult& out) {
        recv(handle, buf, out);
    }
};

struct Uds {
    using Handle = Socket;

    struct BindParams {
        std::string path;
    };

    struct SendParams {
        std::string server_path;
        std::string client_path;
        Buffer payload;
    };

    struct RecvResult {
        sockaddr_un from{};
    };

    static constexpr SocketConfig kCfg{AF_UNIX, SOCK_DGRAM, 0};

    static Handle open() { return Handle(kCfg); }

    static int native_fd(const Handle& handle) { return handle.fd(); }

    static void set_recv_timeout(const Handle& handle, int ms) {
        datagram_set_recv_timeout(handle.fd(), ms);
    }

    static void bind(const Handle& handle, const BindParams& params) {
        datagram_set_reuseaddr(handle.fd());
        ::unlink(params.path.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, params.path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(handle.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("uds bind failed");
        }
    }

    static void send_to(int fd, const char* path, const Buffer& payload) {
        sockaddr_un dest{};
        dest.sun_family = AF_UNIX;
        std::strncpy(dest.sun_path, path, sizeof(dest.sun_path) - 1);

        ssize_t n = ::sendto(fd, payload.data, payload.size, 0,
            reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (n < 0) {
            throw std::runtime_error("uds sendto failed");
        }
    }

    static void send_to(int fd, const std::string& path, const Buffer& payload) {
        send_to(fd, path.c_str(), payload);
    }

    static void send(const Handle& handle, const SendParams& params, const Buffer& payload) {
        if (!params.client_path.empty()) {
            ::unlink(params.client_path.c_str());
            sockaddr_un local{};
            local.sun_family = AF_UNIX;
            std::strncpy(local.sun_path, params.client_path.c_str(), sizeof(local.sun_path) - 1);
            if (::bind(handle.fd(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
                throw std::runtime_error("uds client bind failed");
            }
        }

        send_to(handle.fd(), params.server_path, payload);
    }

    static void recv(const Handle& handle, Buffer& buf, RecvResult& out) {
        socklen_t from_len = sizeof(out.from);

        const ssize_t n = ::recvfrom(handle.fd(), buf.data, buf.capacity, 0,
            reinterpret_cast<sockaddr*>(&out.from), &from_len);

        if (n < 0) {
            throw std::runtime_error("uds recvfrom failed");
        }
        buf.size = static_cast<size_t>(n);
    }

    static void connect_or_send(const Handle& handle, const SendParams& params) {
        send(handle, params, params.payload);
    }

    static void echo(const Handle& handle, const RecvResult& r, const Buffer& payload) {
        send_to(handle.fd(), r.from.sun_path, payload);
    }

    static void recv_from(const Handle& handle, Buffer& buf, RecvResult& out) {
        recv(handle, buf, out);
    }
};
