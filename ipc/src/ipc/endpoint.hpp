#pragma once

#include "transport.hpp"

template<Transport T>
class IpcEndpoint {
public:
    IpcEndpoint() : handle_(T::open()) {}

    IpcEndpoint(IpcEndpoint&&) noexcept = default;
    IpcEndpoint& operator=(IpcEndpoint&&) noexcept = default;
    IpcEndpoint(const IpcEndpoint&) = delete;
    IpcEndpoint& operator=(const IpcEndpoint&) = delete;

    typename T::Handle& handle() { return handle_; }
    const typename T::Handle& handle() const { return handle_; }

    void bind(const typename T::BindParams& params) {
        T::bind(handle_, params);
    }

    void recv(Buffer& buf, typename T::RecvResult& out) {
        T::recv(handle_, buf, out);
    }

    // Non-blocking receive: forwards to T::try_recv when the transport exposes
    // one (datagram sockets via MSG_DONTWAIT). Returns false when no message is
    // available. Used by the Phase H mixed-transport router's cooperative poll.
    bool try_recv(Buffer& buf, typename T::RecvResult& out)
        requires requires(typename T::Handle& h) { T::try_recv(h, buf, out); }
    {
        return T::try_recv(handle_, buf, out);
    }

    void send(const typename T::SendParams& params, const Buffer& payload) {
        T::send(handle_, params, payload);
    }

    // Non-blocking publish: forwards to T::try_send when the transport exposes
    // one (e.g. SPSC ring with backpressure). Only instantiated for transports
    // that declare try_send (see ShmSpsc); use send() for blocking transports.
    auto try_send(const typename T::SendParams& params, const Buffer& payload)
        requires requires(typename T::Handle& h) { T::try_send(h, params, payload); }
    {
        return T::try_send(handle_, params, payload);
    }

    void set_recv_timeout_ms(int ms) {
        if constexpr (requires { T::set_recv_timeout(handle_, ms); }) {
            T::set_recv_timeout(handle_, ms);
        }
    }

    void set_recv_blocking() {
        set_recv_timeout_ms(0);
    }

    // Datagram transports expose a native fd.
    int fd() const
        requires requires(const typename T::Handle& h) { T::native_fd(h); }
    {
        return T::native_fd(handle_);
    }

    void recv_from(Buffer& buf, typename T::RecvResult& out) {
        recv(buf, out);
    }

protected:
    typename T::Handle handle_;
};

// Backward-compatible aliases.
template<Transport T>
using Client = IpcEndpoint<T>;

template<Transport T>
using Server = IpcEndpoint<T>;

using UdpClient = IpcEndpoint<Udp>;
using UdsClient = IpcEndpoint<Uds>;
using UdpServer = IpcEndpoint<Udp>;
using UdsServer = IpcEndpoint<Uds>;
