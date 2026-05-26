#pragma once

#include "buffer.hpp"

#include <concepts>

template<typename T>
concept Transport = requires(
    typename T::Handle handle,
    typename T::BindParams bind_params,
    typename T::SendParams send_params,
    Buffer buf,
    typename T::RecvResult& out) {
    { T::open() } -> std::same_as<typename T::Handle>;
    { T::bind(handle, bind_params) } -> std::same_as<void>;
    { T::recv(handle, buf, out) } -> std::same_as<void>;
    { T::send(handle, send_params, buf) } -> std::same_as<void>;
};

template<typename T>
concept DatagramTransport = Transport<T> && requires(typename T::Handle handle, Buffer buf) {
    { T::native_fd(handle) } -> std::convertible_to<int>;
    { T::set_recv_timeout(handle, 0) } -> std::same_as<void>;
};

// Transports that can exit recv loops without blocking forever (poll or try_recv).
template<typename T>
concept InterruptibleTransport = Transport<T> && (
    requires(typename T::Handle handle, Buffer buf, typename T::RecvResult& out) {
        { T::try_recv(handle, buf, out) } -> std::same_as<bool>;
    } || requires(typename T::Handle handle) {
        { T::set_recv_timeout(handle, 0) } -> std::same_as<void>;
    });
