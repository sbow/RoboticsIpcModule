#pragma once

#include "ipc/app_shutdown.hpp"
#include "router/frame.hpp"
#include "router/routing.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

template<typename L, typename BindParams>
concept RouterLink = requires(
    L link,
    BindParams bind_params,
    RouterFrame& frame,
    const RouteRule* rules,
    size_t rule_count,
    uint64_t timestamp_ns,
    volatile std::sig_atomic_t* stop,
    int poll_ms,
    uint8_t peer_id) {
    { link.bind_router(bind_params) } -> std::same_as<void>;
    { link.set_recv_timeout_ms(poll_ms) } -> std::same_as<void>;
    { link.forward(frame, timestamp_ns, rules, rule_count) } -> std::same_as<ForwardResult>;
    { link.send_to_router(frame) } -> std::same_as<void>;
    { link.recv_message(frame) } -> std::same_as<bool>;
    { link.recv_message_until(peer_id, frame, stop, poll_ms) } -> std::same_as<bool>;
};
