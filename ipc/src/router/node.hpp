#pragma once

#include "router/lifecycle.hpp"
#include "router/link.hpp"
#include "router/link_concept.hpp"
#include "router/shm_router_link.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

struct RouterRunOptions {
    int poll_timeout_ms = 200;
    int idle_exit_ms = 0;
};

template<typename Link>
class RouterServer {
public:
    using BindParams = typename Link::BindParams;

    explicit RouterServer(const RouterTopology& topo)
        : link_(Link::server(topo)) {}

    void bind_router(const BindParams& params) {
        link_.bind_router(params);
    }

    void set_recv_timeout_ms(int ms) {
        link_.set_recv_timeout_ms(ms);
    }

    template<typename OnForward>
    void run(
        const RouteRule* rules,
        size_t rule_count,
        uint64_t (*now_ns)(),
        OnForward on_forward,
        RouterRunOptions opts = {},
        volatile std::sig_atomic_t* stop_flag = router_stop_flag()) {
        link_.set_recv_timeout_ms(opts.poll_timeout_ms);

        RouterFrame frame;
        auto last_activity = std::chrono::steady_clock::now();

        while (!*stop_flag) {
            try {
                const ForwardResult fwd = link_.forward(
                    frame, now_ns(), rules, rule_count);
                if (fwd) {
                    last_activity = std::chrono::steady_clock::now();
                    for (uint8_t dest : fwd.targets) {
                        on_forward(fwd.source, dest, frame);
                    }
                } else if (opts.idle_exit_ms > 0
                    && router_idle_expired(
                        last_activity,
                        std::chrono::milliseconds(opts.idle_exit_ms))) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            } catch (const std::runtime_error&) {
                if (opts.idle_exit_ms > 0
                    && router_idle_expired(
                        last_activity,
                        std::chrono::milliseconds(opts.idle_exit_ms))) {
                    return;
                }
            }
        }
    }

    Link& link() { return link_; }

private:
    Link link_;
};

template<typename Link>
class RouterClient {
public:
    explicit RouterClient(const RouterTopology& topo, uint8_t peer_id)
        : link_(Link::client(topo, peer_id)) {}

    void set_recv_timeout_ms(int ms) { link_.set_recv_timeout_ms(ms); }

    void set_recv_blocking() { link_.set_recv_blocking(); }

    void send_message(uint8_t source_id, const std::string& payload) {
        RouterFrame frame;
        frame.init(source_id);
        frame.set_payload(payload);
        link_.send_to_router(frame);
    }

    bool recv_message(RouterFrame& frame) {
        return link_.recv_message(frame);
    }

    bool recv_message_until(
        uint8_t wanted_source,
        RouterFrame& frame,
        volatile std::sig_atomic_t* stop = router_stop_flag(),
        int poll_timeout_ms = 200) {
        return link_.recv_message_until(wanted_source, frame, stop, poll_timeout_ms);
    }

    Link& link() { return link_; }

private:
    Link link_;
};

template<DatagramTransport Transport>
using DatagramRouterServer = RouterServer<DatagramRouterLink<Transport>>;

template<DatagramTransport Transport>
using DatagramRouterClient = RouterClient<DatagramRouterLink<Transport>>;

using ShmRouterServer = RouterServer<ShmRouterLink>;
using ShmRouterClient = RouterClient<ShmRouterLink>;
