// Phase D1 — peer_id_from_recv<Uds/Udp>
// (ipc/src/router/datagram_peer_resolver.hpp).
//
// The resolver is the layer-2 identity stamp for datagram links: given a
// sockaddr from `recvfrom`, return the peer id that owns that address.
// This unit test fabricates RecvResult instances directly (no socket
// traffic) so we don't depend on the kernel; we just verify the lookup
// over a known topology.

#include "router/datagram_peer_resolver.hpp"
#include "router/peer_table.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/un.h>

namespace {

int g_total  = 0;
int g_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT(" #cond ")\n";                            \
        }                                                                   \
    } while (0)

#define EXPECT_EQ(a, b)                                                     \
    do {                                                                    \
        ++g_total;                                                          \
        const auto _a = (a);                                                \
        const auto _b = (b);                                                \
        if (!(_a == _b)) {                                                  \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT_EQ(" #a ", " #b ") -> "                   \
                      << +(_a) << " != " << +(_b) << "\n";                  \
        }                                                                   \
    } while (0)

// UDS — three peers keyed by sun_path; resolver compares strcmp.

constexpr const char* kSensorUdsPath     = "/tmp/rim_resolver_a.sock";
constexpr const char* kControllerUdsPath = "/tmp/rim_resolver_b.sock";
constexpr const char* kRecorderUdsPath   = "/tmp/rim_resolver_c.sock";
constexpr const char* kRouterUdsPath     = "/tmp/rim_resolver_router.sock";

constexpr PeerEntry kUdsPeers[] = {
    {1, "sensor",     peer_uds(kSensorUdsPath)},
    {2, "controller", peer_uds(kControllerUdsPath)},
    {3, "recorder",   peer_uds(kRecorderUdsPath)},
};

constexpr RouterTopology kUdsTopo = {
    .peers         = kUdsPeers,
    .peer_count    = sizeof(kUdsPeers) / sizeof(kUdsPeers[0]),
    .router_listen = peer_uds(kRouterUdsPath),
};

Uds::RecvResult make_uds_recv(const char* sun_path) {
    Uds::RecvResult r{};
    r.from.sun_family = AF_UNIX;
    if (sun_path) {
        std::strncpy(r.from.sun_path, sun_path, sizeof(r.from.sun_path) - 1);
    }
    return r;
}

void test_uds_resolves_each_known_peer() {
    const auto a = make_uds_recv(kSensorUdsPath);
    const auto b = make_uds_recv(kControllerUdsPath);
    const auto c = make_uds_recv(kRecorderUdsPath);

    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, a), 1);
    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, b), 2);
    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, c), 3);
}

void test_uds_unknown_path_returns_invalid_sentinel() {
    const auto unknown = make_uds_recv("/tmp/rim_resolver_ghost.sock");
    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, unknown), kEndpointInvalid);
}

void test_uds_empty_sun_path_returns_invalid_sentinel() {
    // sun_path zero-initialized → empty C-string; should not match any peer.
    Uds::RecvResult r{};
    r.from.sun_family = AF_UNIX;
    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, r), kEndpointInvalid);
}

void test_uds_partial_prefix_does_not_match() {
    // strcmp is the comparison; a path that is a prefix of a real peer
    // path must NOT match (regression: someone might be tempted to use
    // strncmp in the future).
    const auto prefix = make_uds_recv("/tmp/rim_resolver_a");
    EXPECT_EQ(peer_id_from_recv<Uds>(kUdsTopo, prefix), kEndpointInvalid);
}

// UDP — three peers keyed by port; resolver ignores host on purpose
// (the loopback assumption in the demo topology — see DESIGN-PRINCIPLES).

constexpr PeerEntry kUdpPeers[] = {
    {1, "sensor",     peer_udp("127.0.0.1", 19101)},
    {2, "controller", peer_udp("127.0.0.1", 19102)},
    {3, "recorder",   peer_udp("127.0.0.1", 19103)},
};

constexpr RouterTopology kUdpTopo = {
    .peers         = kUdpPeers,
    .peer_count    = sizeof(kUdpPeers) / sizeof(kUdpPeers[0]),
    .router_listen = peer_udp("127.0.0.1", 19100),
};

Udp::RecvResult make_udp_recv(uint16_t port) {
    Udp::RecvResult r{};
    r.from.sin_family = AF_INET;
    r.from.sin_port   = htons(port);
    r.from.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return r;
}

void test_udp_resolves_each_known_peer() {
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, make_udp_recv(19101)), 1);
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, make_udp_recv(19102)), 2);
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, make_udp_recv(19103)), 3);
}

void test_udp_unknown_port_returns_invalid_sentinel() {
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, make_udp_recv(19999)),
              kEndpointInvalid);
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, make_udp_recv(0)),
              kEndpointInvalid);
}

void test_udp_resolves_on_port_alone_ignores_loopback_host_byte_order() {
    // The resolver compares ntohs(sin_port); the test of htonl(INADDR_LOOPBACK)
    // is informational, not asserted by the function. A fabricated address
    // with a different sin_addr but matching port still resolves to the peer
    // — that's the documented contract on a trusted-LAN deployment
    // (DESIGN-PRINCIPLES "Identity & routing #1").
    Udp::RecvResult elsewhere{};
    elsewhere.from.sin_family      = AF_INET;
    elsewhere.from.sin_port        = htons(19102);
    elsewhere.from.sin_addr.s_addr = htonl(0xC0A80001u);  // 192.168.0.1
    EXPECT_EQ(peer_id_from_recv<Udp>(kUdpTopo, elsewhere), 2);
}

// Mixed-transport topology: a UDP RecvResult must not match a UDS peer
// and vice versa. The resolver branches on Transport (if constexpr), so
// this guards against schema confusion.

constexpr PeerEntry kMixedPeers[] = {
    {1, "uds_peer", peer_uds(kSensorUdsPath)},
    {2, "udp_peer", peer_udp("127.0.0.1", 19101)},
};

constexpr RouterTopology kMixedTopo = {
    .peers         = kMixedPeers,
    .peer_count    = sizeof(kMixedPeers) / sizeof(kMixedPeers[0]),
    .router_listen = peer_udp("127.0.0.1", 19100),
};

void test_uds_resolver_skips_udp_peers() {
    // Address that matches the UDP peer's port — but resolver<Uds> can't
    // see it because the UDP peer's local.kind != UdsPath.
    const auto r = make_uds_recv(kSensorUdsPath);
    EXPECT_EQ(peer_id_from_recv<Uds>(kMixedTopo, r), 1);

    Uds::RecvResult ghost = make_uds_recv("/tmp/some/other.sock");
    EXPECT_EQ(peer_id_from_recv<Uds>(kMixedTopo, ghost), kEndpointInvalid);
}

void test_udp_resolver_skips_uds_peers() {
    EXPECT_EQ(peer_id_from_recv<Udp>(kMixedTopo, make_udp_recv(19101)), 2);

    // Port 0 doesn't match — and the UDS peer in the topology is not visible
    // to the UDP resolver branch at all.
    EXPECT_EQ(peer_id_from_recv<Udp>(kMixedTopo, make_udp_recv(0)),
              kEndpointInvalid);
}

}  // namespace

int main() {
    test_uds_resolves_each_known_peer();
    test_uds_unknown_path_returns_invalid_sentinel();
    test_uds_empty_sun_path_returns_invalid_sentinel();
    test_uds_partial_prefix_does_not_match();

    test_udp_resolves_each_known_peer();
    test_udp_unknown_port_returns_invalid_sentinel();
    test_udp_resolves_on_port_alone_ignores_loopback_host_byte_order();

    test_uds_resolver_skips_udp_peers();
    test_udp_resolver_skips_uds_peers();

    std::cout << "resolver_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
