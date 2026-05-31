#pragma once

// Topic registry — declarative `topic_id` → (name, payload_class,
// sideband_idx) catalog supplied by the deployment profile.
//
// Phase F C5 Scope B — the router today routes by `source` peer id only;
// `topic_id` is just an opaque 16-bit field in the RouterFrame
// (ipc/src/router/frame.hpp). Without a registry every bridge has to
// hard-code magic numbers ("topic 100 means imu_proprio, sideband
// index 0"), which is exactly the schema-drift hazard the parked
// post-phases review flagged.
//
// What the registry does today:
//   * Names every topic_id used in the deployment, with a free-form
//     payload_class string (analogous to SidebandRegion::class — ADR 0005
//     conventions are kSidebandClassVisionNv12 etc., not enforced) and an
//     optional sideband_idx hint.
//   * Lets bridges, recorders and the dashboard validate published frames
//     ("frame.topic_id() == 100; registry says name=imu_proprio,
//     sideband_idx=kSidebandIdxNone, so frame.sideband_idx() must also be
//     kSidebandIdxNone").
//   * Is read by both the C++ router process and Python tooling, so the
//     two stay in sync without sharing a header.
//
// What the registry does NOT do yet (parked as C5 Scope C):
//   * Drive routing decisions. RouteRule is still per-source.
//   * Enforce that publishers and subscribers agree. Validation is
//     opt-in on the consumer side.
//   * Define payload schemas. payload_class is a free string the
//     consumer is expected to recognise; binary layout lives outside
//     this header.
//
// Storage ownership: TopicEntry stores `const char*` interned by the
// topology loader (LoadedTopology's std::deque<std::string>). Lifetime
// matches PeerEntry::name — valid as long as the owning LoadedTopology
// stays alive (and across moves of it — see topology_loader.hpp comment).
//
// Header-only, no allocations, no transport dependency.

#include "router/frame.hpp"
#include "router/peer_table.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Maximum length (excluding the NUL terminator) for topic name /
// payload_class. Picked to keep TopicEntry small and to match the
// peer-name budget — topics, like peers, are short identifiers in
// deployment profiles, not free text.
constexpr size_t kTopicNameMaxLen         = 63;
constexpr size_t kTopicPayloadClassMaxLen = 63;

struct TopicEntry {
    // topic_id wire value (matches the 16-bit field in RouterFrame).
    uint16_t id = 0;

    // Optional sideband slot hint (default kSidebandIdxNone = "no
    // sideband"). When set, declares the canonical sideband index the
    // topic uses on its publishing peer.
    uint16_t sideband_idx = kSidebandIdxNone;

    // Topic name as it appears in the profile (e.g. "imu_proprio",
    // "vision_frame"). Borrowed; storage owned by LoadedTopology.
    const char* name = nullptr;

    // Free-form payload class hint (e.g. "imu_proprio", "vision_nv12").
    // Nullable; the loader stores nullptr when the profile omits the
    // field. The router itself never inspects this string — it exists
    // for bridges, recorders and the dashboard to cross-check published
    // payload shapes against the topic catalog.
    const char* payload_class = nullptr;
};

template<typename Predicate>
inline const TopicEntry* topic_find(const RouterTopology& topo, Predicate pred) {
    for (size_t i = 0; i < topo.topic_count; ++i) {
        if (pred(topo.topics[i])) {
            return &topo.topics[i];
        }
    }
    return nullptr;
}

inline const TopicEntry* topic_by_id(const RouterTopology& topo, uint16_t id) {
    return topic_find(topo, [id](const TopicEntry& e) { return e.id == id; });
}

inline const TopicEntry* topic_by_name(const RouterTopology& topo,
                                       const char* name) {
    if (!name) {
        return nullptr;
    }
    return topic_find(topo, [name](const TopicEntry& e) {
        return e.name != nullptr && std::strcmp(name, e.name) == 0;
    });
}
