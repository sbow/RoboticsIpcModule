#pragma once

// Topology loader — parse a TOML deployment profile into a runtime topology.
//
// Schema (see config/profiles/*.toml for full examples):
//
//   [router]
//   listen     = "uds:/tmp/router.sock" # or "udp:127.0.0.1:19100", "shm:/router_a"
//   listen_uds = "uds:/tmp/router.sock" # Phase H, optional: extra datagram
//   listen_udp = "udp:0.0.0.0:19100"    # listen(s) for a MIXED-transport router
//                                        # (router serves SHM + UDS + UDP peers
//                                        # from one process, ADR 0014). SHM peers
//                                        # need no listen; each datagram peer
//                                        # requires a matching listen here.
//
//   [[peers]]
//   id              = 1
//   name            = "sensor"
//   local           = "uds:/tmp/sensor.sock"
//   shm_slot_count  = 256                # ADR 0009, optional, shm: peers only
//   shm_max_payload = 64                 # ADR 0009, optional, shm: peers only
//                                        # >= kRouterFrameSize (64), <= 256 MiB
//
//     [[peers.sideband]]                # optional, per ADR 0005
//     class             = "vision_nv12"
//     name              = "/robot_vision_nv12"
//     max_payload_bytes = 8388608
//     memory_class      = "nvbufsurface" # ADR 0008/0012, optional, default "shm";
//                                        # one of: shm, cuda_managed, cuda_host,
//                                        # nvbufsurface
//     cuda_device       = 0              # optional, GPU-backed classes only;
//                                        # rejected on a shm region
//
//   [[routes]]
//   source = 1
//   dest   = [2, 3, 8]                  # 1..kMaxRouteDests destinations
//                                        # (C5 Scope A lifted the prior 2-dest cap)
//   topic  = 100                        # Phase G, optional; restricts the rule
//                                        # to frames with this topic_id. Omitted
//                                        # => match any topic (source-only).
//                                        # Must reference a [[topics]] id; 0xFFFF
//                                        # reserved as the match-any sentinel.
//
//   [[topics]]                          # Phase F C5 Scope B, optional
//   id            = 100                 # uint16, required, unique
//   name          = "imu_proprio"       # required, <= 63 bytes
//   payload_class = "imu_proprio"       # optional, free-form, <= 63 bytes
//   sideband_idx  = 0                   # optional uint16, default kSidebandIdxNone
//
//   Topics are declarative only — the router does not consult the
//   registry; bridges and tooling use it to validate published frames.
//
// Layering: this header lives inside ipc/src/router/ but is NOT pulled in by
// router_protocol.hpp on purpose — apps that don't want the TOML dependency
// can keep using compile-time topologies. To use the loader, include this
// header explicitly and link with -Ithird_party/tomlplusplus.
//
// Storage ownership:
//   * LoadedTopology owns all strings it parses (in a std::deque<std::string>,
//     which keeps element references stable on push_back).
//   * It is **non-copyable but movable**. On Linux (libstdc++ / libc++), moving
//     a std::deque<std::string> and std::vector<T> preserves element addresses
//     (deque nodes transfer; vector heap buffer transfers), so the interned
//     const char* pointers inside RouterTopology / PeerEntry / SidebandRegion
//     remain valid across moves. Do NOT copy — copying would re-allocate the
//     deque and invalidate every interned pointer.

#include "router/frame.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/sideband.hpp"
#include "router/topic_table.hpp"

#include <toml.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class TopologyLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace topology_loader_detail {

// Bound used in errors; tracks the Linux struct sockaddr_un sun_path limit.
constexpr size_t kUdsPathMaxLen = 107;
constexpr size_t kPeerNameMaxLen = 63;
constexpr size_t kShmNameMaxLen = 250;

// ADR 0009 — per-peer SHM ring sizing limits enforced at load time.
constexpr int64_t kShmSlotCountMin       = 1;
constexpr int64_t kShmSlotCountMax       = 1 << 20;          // ~1M slots
constexpr int64_t kShmMaxPayloadMax      = 256 * 1024 * 1024; // 256 MiB

inline void throw_load(const std::string& what) {
    throw TopologyLoadError(what);
}

inline std::pair<std::string_view, std::string_view> split_scheme(
    std::string_view spec) {
    const auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        throw_load("address missing scheme: '" + std::string(spec)
                   + "' (expected uds:/path, udp:host:port, or shm:/name)");
    }
    return {spec.substr(0, colon), spec.substr(colon + 1)};
}

}  // namespace topology_loader_detail

class LoadedTopology {
public:
    LoadedTopology() = default;

    LoadedTopology(const LoadedTopology&) = delete;
    LoadedTopology& operator=(const LoadedTopology&) = delete;
    LoadedTopology(LoadedTopology&&) noexcept            = default;
    LoadedTopology& operator=(LoadedTopology&&) noexcept = default;

    // RouterTopology view — pointers into this LoadedTopology's storage.
    RouterTopology view() const noexcept {
        RouterTopology v{};
        v.peers          = peers_.data();
        v.peer_count     = peers_.size();
        v.router_listen  = router_listen_;
        v.topics         = topics_.data();
        v.topic_count    = topics_.size();
        v.has_listen_uds = has_listen_uds_;
        v.listen_uds     = listen_uds_;
        v.has_listen_udp = has_listen_udp_;
        v.listen_udp     = listen_udp_;
        return v;
    }

    const RouteRule* routes() const noexcept { return routes_.data(); }
    size_t route_count() const noexcept { return routes_.size(); }

    const TopicEntry* topics() const noexcept { return topics_.data(); }
    size_t topic_count() const noexcept { return topics_.size(); }

    // Per-peer sideband descriptors (may be empty).
    // out_count is set even on empty (to 0); return value may be nullptr.
    const SidebandRegion* sidebands_for(uint8_t peer_id,
                                        size_t& out_count) const noexcept {
        out_count = 0;
        const auto* peer = peer_by_id(view(), peer_id);
        if (!peer) {
            return nullptr;
        }
        const size_t idx = static_cast<size_t>(peer - peers_.data());
        if (idx >= peer_sideband_ranges_.size()) {
            return nullptr;
        }
        const auto& range = peer_sideband_ranges_[idx];
        out_count = range.second - range.first;
        return out_count == 0 ? nullptr : (sidebands_.data() + range.first);
    }

    PeerAddress router_listen() const noexcept { return router_listen_; }

    // Friend factory functions need write access. Everything else is read-only.
    friend LoadedTopology load_topology_from_toml_string(std::string_view toml);
    friend LoadedTopology load_topology_from_toml_file(const std::string& path);

private:
    // Intern a string by copying into the owned deque; returns a stable pointer.
    const char* intern_(std::string_view s) {
        strings_.emplace_back(s);
        return strings_.back().c_str();
    }

    PeerAddress parse_address_(std::string_view spec);

    void build_from_(const toml::table& root);

    std::deque<std::string> strings_;
    std::vector<PeerEntry>  peers_;
    std::vector<RouteRule>  routes_;
    std::vector<TopicEntry> topics_;
    std::vector<SidebandRegion> sidebands_;
    std::vector<std::pair<size_t, size_t>> peer_sideband_ranges_;  // by peer index
    PeerAddress router_listen_{};

    // Phase H — per-datagram-transport router listen endpoints for mixed
    // profiles. Resolved from [router].listen (when it is a datagram address)
    // and/or the optional [router].listen_uds / listen_udp keys.
    bool has_listen_uds_ = false;
    PeerAddress listen_uds_{};
    bool has_listen_udp_ = false;
    PeerAddress listen_udp_{};
};

inline PeerAddress LoadedTopology::parse_address_(std::string_view spec) {
    using namespace topology_loader_detail;
    const auto [scheme, rest] = split_scheme(spec);

    if (scheme == "uds") {
        if (rest.empty() || rest[0] != '/') {
            throw_load("uds address must be an absolute path: '"
                       + std::string(spec) + "'");
        }
        if (rest.size() > kUdsPathMaxLen) {
            throw_load("uds path exceeds " + std::to_string(kUdsPathMaxLen)
                       + " bytes: '" + std::string(rest) + "'");
        }
        return peer_uds(intern_(rest));
    }

    if (scheme == "udp") {
        const auto colon = rest.rfind(':');
        if (colon == std::string_view::npos || colon == 0
         || colon + 1 == rest.size()) {
            throw_load("udp address must be host:port (no IPv6 brackets): '"
                       + std::string(spec) + "'");
        }
        const std::string_view host = rest.substr(0, colon);
        const std::string_view port_sv = rest.substr(colon + 1);
        if (host.find(':') != std::string_view::npos) {
            throw_load("udp host must not contain ':' (IPv6 not supported in "
                       "v1 topology schema): '" + std::string(host) + "'");
        }
        unsigned long port = 0;
        try {
            port = std::stoul(std::string(port_sv));
        } catch (const std::exception&) {
            throw_load("udp port not an integer: '" + std::string(port_sv) + "'");
        }
        if (port == 0 || port > 65535) {
            throw_load("udp port out of range 1..65535: " + std::to_string(port));
        }
        return peer_udp(intern_(host), static_cast<uint16_t>(port));
    }

    if (scheme == "shm") {
        if (rest.empty() || rest[0] != '/') {
            throw_load("shm address must start with '/': '"
                       + std::string(spec) + "'");
        }
        if (rest.size() > kShmNameMaxLen) {
            throw_load("shm name exceeds " + std::to_string(kShmNameMaxLen)
                       + " bytes: '" + std::string(rest) + "'");
        }
        return peer_shm(intern_(rest));
    }

    throw_load("unknown address scheme '" + std::string(scheme)
               + "' (want uds, udp, or shm): '" + std::string(spec) + "'");
    return {};   // unreachable
}

inline void LoadedTopology::build_from_(const toml::table& root) {
    using namespace topology_loader_detail;

    LoadedTopology& out = *this;
    // [router].listen
    auto* router_section = root["router"].as_table();
    if (!router_section) {
        throw_load("missing [router] section");
    }
    auto listen_str = (*router_section)["listen"].value<std::string>();
    if (!listen_str) {
        throw_load("missing router.listen string");
    }
    out.router_listen_ = out.parse_address_(*listen_str);

    // Phase H — multi-listen for the mixed-transport router. The primary
    // `listen` stays required and back-compatible: when it is a datagram
    // address it is also the listen for that datagram transport. A mixed
    // profile adds optional `listen_uds` / `listen_udp` keys for the other
    // datagram transport(s) it serves. SHM peers need no listen (per-peer
    // rings are derived from the topology).
    if (out.router_listen_.kind == PeerAddressKind::UdsPath) {
        out.has_listen_uds_ = true;
        out.listen_uds_     = out.router_listen_;
    } else if (out.router_listen_.kind == PeerAddressKind::UdpEndpoint) {
        out.has_listen_udp_ = true;
        out.listen_udp_     = out.router_listen_;
    }
    if (auto uds_str = (*router_section)["listen_uds"].value<std::string>()) {
        PeerAddress addr = out.parse_address_(*uds_str);
        if (addr.kind != PeerAddressKind::UdsPath) {
            throw_load("router.listen_uds must be a uds: address");
        }
        out.has_listen_uds_ = true;
        out.listen_uds_     = addr;
    }
    if (auto udp_str = (*router_section)["listen_udp"].value<std::string>()) {
        PeerAddress addr = out.parse_address_(*udp_str);
        if (addr.kind != PeerAddressKind::UdpEndpoint) {
            throw_load("router.listen_udp must be a udp: address");
        }
        out.has_listen_udp_ = true;
        out.listen_udp_     = addr;
    }

    // [[peers]]
    auto* peers_array = root["peers"].as_array();
    if (!peers_array || peers_array->empty()) {
        throw_load("missing or empty [[peers]] array (need at least one peer)");
    }

    out.peer_sideband_ranges_.reserve(peers_array->size());

    for (size_t i = 0; i < peers_array->size(); ++i) {
        auto* peer_tbl = peers_array->at(i).as_table();
        if (!peer_tbl) {
            throw_load("[[peers]] entry #" + std::to_string(i)
                       + " is not a table");
        }

        auto id_opt = (*peer_tbl)["id"].value<int64_t>();
        if (!id_opt) {
            throw_load("[[peers]] entry #" + std::to_string(i)
                       + " missing integer 'id'");
        }
        if (*id_opt < 1 || *id_opt > 254) {
            throw_load("peer id " + std::to_string(*id_opt)
                       + " out of range 1..254 (0=invalid, 255=server reserved)");
        }
        const uint8_t id = static_cast<uint8_t>(*id_opt);

        auto name_opt = (*peer_tbl)["name"].value<std::string>();
        if (!name_opt) {
            throw_load("peer id " + std::to_string(id)
                       + " missing string 'name'");
        }
        if (name_opt->size() > kPeerNameMaxLen) {
            throw_load("peer name '" + *name_opt + "' exceeds "
                       + std::to_string(kPeerNameMaxLen) + " bytes");
        }
        if (name_opt->empty()) {
            throw_load("peer id " + std::to_string(id) + " has empty name");
        }

        auto local_opt = (*peer_tbl)["local"].value<std::string>();
        if (!local_opt) {
            throw_load("peer '" + *name_opt + "' missing string 'local'");
        }

        // Duplicate id check
        for (const auto& existing : out.peers_) {
            if (existing.id == id) {
                throw_load("duplicate peer id " + std::to_string(id));
            }
        }

        PeerEntry entry;
        entry.id    = id;
        entry.name  = out.intern_(*name_opt);
        entry.local = out.parse_address_(*local_opt);

        // ADR 0009 — optional per-peer SHM ring sizing.
        auto slot_count_opt  = (*peer_tbl)["shm_slot_count"].value<int64_t>();
        auto max_payload_opt = (*peer_tbl)["shm_max_payload"].value<int64_t>();
        const bool has_shm_override = slot_count_opt.has_value()
                                   || max_payload_opt.has_value();
        if (has_shm_override
            && entry.local.kind != PeerAddressKind::ShmRing) {
            throw_load("peer '" + *name_opt + "' has shm_slot_count or "
                       "shm_max_payload but local address is not shm:");
        }
        if (slot_count_opt) {
            const int64_t v = *slot_count_opt;
            if (v != 0 && (v < kShmSlotCountMin || v > kShmSlotCountMax)) {
                throw_load("peer '" + *name_opt + "' shm_slot_count "
                           + std::to_string(v) + " out of range "
                           + std::to_string(kShmSlotCountMin) + ".."
                           + std::to_string(kShmSlotCountMax)
                           + " (0 means default)");
            }
            entry.shm_slot_count = static_cast<uint32_t>(v);
        }
        if (max_payload_opt) {
            const int64_t v = *max_payload_opt;
            if (v != 0 && v < static_cast<int64_t>(kRouterFrameSize)) {
                throw_load("peer '" + *name_opt + "' shm_max_payload "
                           + std::to_string(v) + " < kRouterFrameSize ("
                           + std::to_string(kRouterFrameSize)
                           + "): cannot hold a RouterFrame");
            }
            if (v < 0 || v > kShmMaxPayloadMax) {
                throw_load("peer '" + *name_opt + "' shm_max_payload "
                           + std::to_string(v) + " out of range 0.."
                           + std::to_string(kShmMaxPayloadMax)
                           + " (0 means default)");
            }
            entry.shm_max_payload = static_cast<uint32_t>(v);
        }

        out.peers_.push_back(entry);

        // [[peers.sideband]] (optional, may appear multiple times per peer)
        const size_t sideband_begin = out.sidebands_.size();
        if (auto* sb_array = (*peer_tbl)["sideband"].as_array()) {
            for (size_t j = 0; j < sb_array->size(); ++j) {
                auto* sb_tbl = sb_array->at(j).as_table();
                if (!sb_tbl) {
                    throw_load("peer '" + *name_opt + "' sideband entry #"
                               + std::to_string(j) + " is not a table");
                }
                auto sb_name = (*sb_tbl)["name"].value<std::string>();
                auto sb_bytes = (*sb_tbl)["max_payload_bytes"].value<int64_t>();
                if (!sb_name) {
                    throw_load("peer '" + *name_opt
                               + "' sideband missing string 'name'");
                }
                if (sb_name->size() > kSidebandNameMaxLen) {
                    throw_load("sideband name '" + *sb_name + "' exceeds "
                               + std::to_string(kSidebandNameMaxLen) + " bytes");
                }
                if (!sb_bytes || *sb_bytes <= 0) {
                    throw_load("peer '" + *name_opt
                               + "' sideband '" + *sb_name
                               + "' needs positive max_payload_bytes");
                }
                SidebandRegion region;
                region.name              = out.intern_(*sb_name);
                region.max_payload_bytes = static_cast<size_t>(*sb_bytes);
                region.version           = kSidebandVersion;
                if (auto v = (*sb_tbl)["version"].value<int64_t>()) {
                    if (*v < 0 || *v > 0xffffffff) {
                        throw_load("sideband '" + *sb_name
                                   + "' version out of range");
                    }
                    region.version = static_cast<uint32_t>(*v);
                }
                // class is informational; we don't enforce a whitelist
                // (ADR 0005 §2: the kSidebandClass* constants are conventions).

                // memory_class (ADR 0008 forward declaration, realized in
                // ADR 0012 / F5). Optional; defaults to shm (CPU). Unknown
                // spellings are a hard error so a typo can't silently fall
                // back to the CPU path and break a GPU consumer.
                region.memory_class = SidebandMemoryClass::Shm;
                if (auto mc = (*sb_tbl)["memory_class"].value<std::string>()) {
                    if (!parse_sideband_memory_class(mc->c_str(),
                                                     region.memory_class)) {
                        throw_load("sideband '" + *sb_name
                                   + "' has unknown memory_class '" + *mc
                                   + "' (expected one of: shm, cuda_managed, "
                                     "cuda_host, nvbufsurface)");
                    }
                }

                // cuda_device (optional). Only meaningful for GPU-backed
                // classes; on a `shm` region it is a configuration error so
                // operators don't believe a CPU region is pinned to a GPU.
                region.cuda_device = kSidebandCudaDeviceUnset;
                if (auto dev = (*sb_tbl)["cuda_device"].value<int64_t>()) {
                    if (!sideband_memory_class_is_gpu(region.memory_class)) {
                        throw_load("sideband '" + *sb_name
                                   + "' sets cuda_device but memory_class is '"
                                   + sideband_memory_class_name(region.memory_class)
                                   + "' (cuda_device is only valid for GPU-backed "
                                     "classes: cuda_managed, cuda_host, nvbufsurface)");
                    }
                    if (*dev < 0 || *dev > 255) {
                        throw_load("sideband '" + *sb_name
                                   + "' cuda_device " + std::to_string(*dev)
                                   + " out of range 0..255");
                    }
                    region.cuda_device = static_cast<int>(*dev);
                }

                out.sidebands_.push_back(region);
            }
        }
        const size_t sideband_end = out.sidebands_.size();
        out.peer_sideband_ranges_.emplace_back(sideband_begin, sideband_end);
    }

    // Phase H — a datagram peer can only be served if the router actually
    // listens on that transport. SHM peers need no listen (per-peer rings).
    // This catches the most common mixed-profile mistake: declaring a uds/udp
    // peer but forgetting the matching [router] listen, which would otherwise
    // only surface as silent unknown-source drops at runtime.
    for (const auto& peer : out.peers_) {
        if (peer.local.kind == PeerAddressKind::UdsPath && !out.has_listen_uds_) {
            throw_load("peer '" + std::string(peer.name)
                       + "' is a uds peer but [router] has no uds listen "
                         "(set listen or listen_uds to a uds: address)");
        }
        if (peer.local.kind == PeerAddressKind::UdpEndpoint
            && !out.has_listen_udp_) {
            throw_load("peer '" + std::string(peer.name)
                       + "' is a udp peer but [router] has no udp listen "
                         "(set listen or listen_udp to a udp: address)");
        }
    }

    // [[routes]]
    if (auto* routes_array = root["routes"].as_array()) {
        for (size_t i = 0; i < routes_array->size(); ++i) {
            auto* rt = routes_array->at(i).as_table();
            if (!rt) {
                throw_load("[[routes]] entry #" + std::to_string(i)
                           + " is not a table");
            }
            auto source_opt = (*rt)["source"].value<int64_t>();
            if (!source_opt) {
                throw_load("[[routes]] entry #" + std::to_string(i)
                           + " missing integer 'source'");
            }
            auto* dest_arr = (*rt)["dest"].as_array();
            if (!dest_arr || dest_arr->empty()
             || dest_arr->size() > kMaxRouteDests) {
                throw_load("[[routes]] entry #" + std::to_string(i)
                           + " 'dest' must be an array of 1.."
                           + std::to_string(kMaxRouteDests) + " peer ids");
            }
            auto extract_id = [&](size_t k) -> uint8_t {
                auto v = dest_arr->at(k).value<int64_t>();
                if (!v) {
                    throw_load("[[routes]] entry #" + std::to_string(i)
                               + " dest must be integers");
                }
                if (*v < 1 || *v > 254) {
                    throw_load("[[routes]] entry #" + std::to_string(i)
                               + " dest id " + std::to_string(*v)
                               + " out of range 1..254");
                }
                return static_cast<uint8_t>(*v);
            };
            RouteRule rule{};
            rule.source = static_cast<uint8_t>(*source_opt);
            rule.dest_count = static_cast<uint8_t>(dest_arr->size());
            for (size_t k = 0; k < dest_arr->size(); ++k) {
                rule.dest[k] = extract_id(k);
            }

            // Phase G — optional per-topic selector. Absent => kRouteTopicAny
            // (match any topic = legacy source-only behavior). The referenced
            // topic id must exist in [[topics]]; that cross-section check runs
            // as a second pass below, because [[topics]] is parsed after
            // [[routes]] (a route may name a topic declared lower in the file).
            rule.topic_id = kRouteTopicAny;
            if (auto topic_opt = (*rt)["topic"].value<int64_t>()) {
                if (*topic_opt < 0 || *topic_opt > 0xFFFE) {
                    throw_load("[[routes]] entry #" + std::to_string(i)
                               + " topic id " + std::to_string(*topic_opt)
                               + " out of range 0..65534 "
                                 "(0xFFFF reserved as match-any)");
                }
                rule.topic_id = static_cast<uint16_t>(*topic_opt);
            }

            // Validate referenced peers exist
            auto peer_exists = [&](uint8_t pid) {
                return std::any_of(out.peers_.begin(), out.peers_.end(),
                                   [pid](const PeerEntry& p) { return p.id == pid; });
            };
            if (!peer_exists(rule.source)) {
                throw_load("route source id " + std::to_string(rule.source)
                           + " does not match any peer");
            }
            for (size_t k = 0; k < rule.dest_count; ++k) {
                if (!peer_exists(rule.dest[k])) {
                    throw_load("route dest id "
                               + std::to_string(rule.dest[k])
                               + " does not match any peer");
                }
                // Reject duplicate destinations within a single rule. The
                // router would otherwise copy the same frame to the same
                // peer twice, which is almost certainly a profile typo
                // (and trips drop-attribution metrics for SHM rings).
                for (size_t j = 0; j < k; ++j) {
                    if (rule.dest[j] == rule.dest[k]) {
                        throw_load("[[routes]] entry #" + std::to_string(i)
                                   + " duplicate dest id "
                                   + std::to_string(rule.dest[k])
                                   + " in fan-out list");
                    }
                }
                // Phase D1 — reject self-routing. The router has no
                // reason to copy a frame back to the peer that produced
                // it; that is almost certainly a profile mistake.
                if (rule.dest[k] == rule.source) {
                    throw_load("route source id "
                               + std::to_string(rule.source)
                               + " cannot be a destination of itself "
                               "(self-routing rejected)");
                }
            }
            out.routes_.push_back(rule);
        }
    }

    // [[topics]] — Phase F C5 Scope B, optional. Declarative-only; the
    // router never consults this catalog at runtime, so an absent section
    // is a normal deployment.
    if (auto* topics_array = root["topics"].as_array()) {
        for (size_t i = 0; i < topics_array->size(); ++i) {
            auto* tt = topics_array->at(i).as_table();
            if (!tt) {
                throw_load("[[topics]] entry #" + std::to_string(i)
                           + " is not a table");
            }

            auto id_opt = (*tt)["id"].value<int64_t>();
            if (!id_opt) {
                throw_load("[[topics]] entry #" + std::to_string(i)
                           + " missing integer 'id'");
            }
            if (*id_opt < 0 || *id_opt > 0xFFFF) {
                throw_load("[[topics]] entry #" + std::to_string(i)
                           + " id " + std::to_string(*id_opt)
                           + " out of range 0..65535 (u16 wire field)");
            }
            const uint16_t id = static_cast<uint16_t>(*id_opt);

            for (const auto& existing : out.topics_) {
                if (existing.id == id) {
                    throw_load("duplicate topic id " + std::to_string(id));
                }
            }

            auto name_opt = (*tt)["name"].value<std::string>();
            if (!name_opt) {
                throw_load("topic id " + std::to_string(id)
                           + " missing string 'name'");
            }
            if (name_opt->empty()) {
                throw_load("topic id " + std::to_string(id) + " has empty name");
            }
            if (name_opt->size() > kTopicNameMaxLen) {
                throw_load("topic name '" + *name_opt + "' exceeds "
                           + std::to_string(kTopicNameMaxLen) + " bytes");
            }
            // Topic names must also be unique. Bridges look up topics
            // both ways (by id from the wire, by name from
            // configuration), so a clash here would silently mask the
            // duplicate.
            for (const auto& existing : out.topics_) {
                if (existing.name != nullptr
                 && std::strcmp(existing.name, name_opt->c_str()) == 0) {
                    throw_load("duplicate topic name '" + *name_opt + "'");
                }
            }

            TopicEntry topic{};
            topic.id   = id;
            topic.name = out.intern_(*name_opt);

            if (auto pc_opt = (*tt)["payload_class"].value<std::string>()) {
                if (pc_opt->empty()) {
                    throw_load("topic '" + *name_opt
                               + "' has empty payload_class");
                }
                if (pc_opt->size() > kTopicPayloadClassMaxLen) {
                    throw_load("topic '" + *name_opt
                               + "' payload_class exceeds "
                               + std::to_string(kTopicPayloadClassMaxLen)
                               + " bytes");
                }
                topic.payload_class = out.intern_(*pc_opt);
            }

            topic.sideband_idx = kSidebandIdxNone;
            if (auto sb_opt = (*tt)["sideband_idx"].value<int64_t>()) {
                if (*sb_opt < 0 || *sb_opt > 0xFFFF) {
                    throw_load("topic '" + *name_opt
                               + "' sideband_idx " + std::to_string(*sb_opt)
                               + " out of range 0..65535");
                }
                topic.sideband_idx = static_cast<uint16_t>(*sb_opt);
            }

            out.topics_.push_back(topic);
        }
    }

    // Phase G — per-topic route validation (second pass). A route that names
    // a `topic` selector must reference an id declared in [[topics]]. Done
    // after the topics block because routes are parsed first; this mirrors
    // the inline "route dest does not match any peer" rejection, just across
    // sections. Routes with kRouteTopicAny (no selector) are skipped.
    for (size_t i = 0; i < out.routes_.size(); ++i) {
        const uint16_t tid = out.routes_[i].topic_id;
        if (tid == kRouteTopicAny) {
            continue;
        }
        const bool topic_declared = std::any_of(
            out.topics_.begin(), out.topics_.end(),
            [tid](const TopicEntry& t) { return t.id == tid; });
        if (!topic_declared) {
            throw_load("route topic id " + std::to_string(tid)
                       + " does not match any [[topics]] entry");
        }
    }
}

inline LoadedTopology load_topology_from_toml_string(std::string_view toml) {
    using namespace topology_loader_detail;
    toml::table root;
    try {
        root = toml::parse(toml);
    } catch (const toml::parse_error& e) {
        throw_load(std::string("toml parse error: ") + e.what());
    }
    LoadedTopology out;
    out.build_from_(root);
    return out;
}

inline LoadedTopology load_topology_from_toml_file(const std::string& path) {
    using namespace topology_loader_detail;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw_load("toml file '" + path + "': " + e.what());
    }
    LoadedTopology out;
    out.build_from_(root);
    return out;
}
