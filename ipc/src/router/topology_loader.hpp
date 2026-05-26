#pragma once

// Topology loader — parse a TOML deployment profile into a runtime topology.
//
// Schema (see config/profiles/*.toml for full examples):
//
//   [router]
//   listen = "uds:/tmp/router.sock"     # or "udp:127.0.0.1:19100", "shm:/router_a"
//
//   [[peers]]
//   id    = 1
//   name  = "sensor"
//   local = "uds:/tmp/sensor.sock"
//
//     [[peers.sideband]]                # optional, per ADR 0005
//     class             = "vision_nv12"
//     name              = "/robot_vision_nv12"
//     max_payload_bytes = 8388608
//
//   [[routes]]
//   source = 1
//   dest   = [2, 3]                     # 1 or 2 destinations
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
        return RouterTopology{peers_.data(), peers_.size(), router_listen_};
    }

    const RouteRule* routes() const noexcept { return routes_.data(); }
    size_t route_count() const noexcept { return routes_.size(); }

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
    std::vector<SidebandRegion> sidebands_;
    std::vector<std::pair<size_t, size_t>> peer_sideband_ranges_;  // by peer index
    PeerAddress router_listen_{};
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
                out.sidebands_.push_back(region);
            }
        }
        const size_t sideband_end = out.sidebands_.size();
        out.peer_sideband_ranges_.emplace_back(sideband_begin, sideband_end);
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
            if (!dest_arr || dest_arr->empty() || dest_arr->size() > 2) {
                throw_load("[[routes]] entry #" + std::to_string(i)
                           + " 'dest' must be an array of 1 or 2 peer ids");
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
            RouteRule rule;
            rule.source = static_cast<uint8_t>(*source_opt);
            rule.dest0  = extract_id(0);
            rule.dest1  = dest_arr->size() == 2 ? extract_id(1) : uint8_t{0};

            // Validate referenced peers exist
            auto peer_exists = [&](uint8_t pid) {
                return std::any_of(out.peers_.begin(), out.peers_.end(),
                                   [pid](const PeerEntry& p) { return p.id == pid; });
            };
            if (!peer_exists(rule.source)) {
                throw_load("route source id " + std::to_string(rule.source)
                           + " does not match any peer");
            }
            if (!peer_exists(rule.dest0)) {
                throw_load("route dest id " + std::to_string(rule.dest0)
                           + " does not match any peer");
            }
            if (rule.dest1 != 0 && !peer_exists(rule.dest1)) {
                throw_load("route dest id " + std::to_string(rule.dest1)
                           + " does not match any peer");
            }
            out.routes_.push_back(rule);
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
